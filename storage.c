#include <stdint.h>

#include "ata.h"
#include "storage.h"

#define STORAGE_MAGIC 0x48535053u /* "HSPS" */
#define STORAGE_SLOT_COUNT 2u
#define STORAGE_SERIALIZED_PAYLOAD_SIZE 5808u

static uint8_t storage_initialized;
static int32_t storage_active_slot = -1;
static uint32_t storage_current_generation;
static uint8_t storage_slot_buffer[STORAGE_SLOT_BYTES];
static uint8_t storage_expected_payload[STORAGE_SERIALIZED_PAYLOAD_SIZE];
static struct hospital_storage_state storage_candidate;
static struct hospital_storage_state storage_best;

static int storage_slot_lba(uint32_t slot, uint32_t *lba) {
    if (lba == (void *)0 || slot >= STORAGE_SLOT_COUNT) {
        return STORAGE_ERR_ARGUMENT;
    }
    *lba = slot == 0 ? STORAGE_SLOT_A_LBA : STORAGE_SLOT_B_LBA;
    return STORAGE_OK;
}

static uint8_t storage_layout_valid(void) {
    uint32_t tail_end = STORAGE_SELF_TEST_RESERVED_FIRST_LBA +
                        STORAGE_SELF_TEST_RESERVED_SECTORS;
    uint32_t slot_a_end = STORAGE_SLOT_A_LBA + STORAGE_SLOT_SECTORS;
    uint32_t slot_b_end = STORAGE_SLOT_B_LBA + STORAGE_SLOT_SECTORS;

    if (STORAGE_SLOT_BYTES < STORAGE_HEADER_SIZE ||
        STORAGE_SERIALIZED_PAYLOAD_SIZE > STORAGE_PAYLOAD_CAPACITY) {
        return 0;
    }
    if (slot_a_end > STORAGE_TEST_DISK_SECTORS ||
        slot_b_end > STORAGE_TEST_DISK_SECTORS ||
        STORAGE_SLOT_A_LBA < STORAGE_HEADER_SIZE ||
        slot_a_end > STORAGE_SLOT_B_LBA ||
        (STORAGE_SLOT_A_LBA < tail_end &&
         slot_a_end > STORAGE_SELF_TEST_RESERVED_FIRST_LBA) ||
        (STORAGE_SLOT_B_LBA < tail_end &&
         slot_b_end > STORAGE_SELF_TEST_RESERVED_FIRST_LBA) ||
        STORAGE_SELF_TEST_LBA < STORAGE_SELF_TEST_RESERVED_FIRST_LBA ||
        STORAGE_SELF_TEST_LBA >= tail_end) {
        return 0;
    }
    return 1;
}

static uint8_t storage_put_u32(uint8_t *buffer, uint32_t *offset,
                               uint32_t limit, uint32_t value) {
    if (buffer == (void *)0 || offset == (void *)0 || *offset > limit ||
        limit - *offset < 4u) {
        return 0;
    }
    buffer[*offset + 0] = (uint8_t)value;
    buffer[*offset + 1] = (uint8_t)(value >> 8);
    buffer[*offset + 2] = (uint8_t)(value >> 16);
    buffer[*offset + 3] = (uint8_t)(value >> 24);
    *offset += 4u;
    return 1;
}

static uint8_t storage_get_u32(const uint8_t *buffer, uint32_t *offset,
                               uint32_t limit, uint32_t *value) {
    if (buffer == (void *)0 || offset == (void *)0 || value == (void *)0 ||
        *offset > limit || limit - *offset < 4u) {
        return 0;
    }
    *value = (uint32_t)buffer[*offset + 0] |
                     ((uint32_t)buffer[*offset + 1] << 8) |
                     ((uint32_t)buffer[*offset + 2] << 16) |
                     ((uint32_t)buffer[*offset + 3] << 24);
    *offset += 4u;
    return 1;
}

static uint8_t storage_put_bytes(uint8_t *buffer, uint32_t *offset,
                                 uint32_t limit, const void *source,
                                 uint32_t length) {
    if (buffer == (void *)0 || offset == (void *)0 || source == (void *)0 ||
        *offset > limit || length > limit - *offset) {
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)source;
    for (uint32_t index = 0; index < length; index++) {
        buffer[*offset + index] = bytes[index];
    }
    *offset += length;
    return 1;
}

static uint8_t storage_get_bytes(const uint8_t *buffer, uint32_t *offset,
                                 uint32_t limit, void *destination,
                                 uint32_t length) {
    if (buffer == (void *)0 || offset == (void *)0 ||
        destination == (void *)0 || *offset > limit ||
        length > limit - *offset) {
        return 0;
    }
    uint8_t *bytes = (uint8_t *)destination;
    for (uint32_t index = 0; index < length; index++) {
        bytes[index] = buffer[*offset + index];
    }
    *offset += length;
    return 1;
}

static uint8_t storage_string_valid(const char *value, uint32_t capacity) {
    if (value[0] == '\0') {
        return 0;
    }
    for (uint32_t index = 1; index < capacity; index++) {
        if (value[index] == '\0') {
            return 1;
        }
    }
    return 0;
}

static uint8_t storage_patient_exists(const struct hospital_storage_state *state,
                                      uint32_t id) {
    for (uint32_t index = 0; index < STORAGE_MAX_PATIENTS; index++) {
        if (state->patients[index].in_use && state->patients[index].id == id) {
            return 1;
        }
    }
    return 0;
}

static uint8_t storage_doctor_exists(const struct hospital_storage_state *state,
                                     uint32_t id) {
    for (uint32_t index = 0; index < STORAGE_MAX_DOCTORS; index++) {
        if (state->doctors[index].in_use && state->doctors[index].id == id) {
            return 1;
        }
    }
    return 0;
}

static uint8_t storage_unique_patient_ids(const struct hospital_storage_state *state) {
    for (uint32_t left = 0; left < STORAGE_MAX_PATIENTS; left++) {
        if (!state->patients[left].in_use) {
            continue;
        }
        for (uint32_t right = left + 1; right < STORAGE_MAX_PATIENTS; right++) {
            if (state->patients[right].in_use &&
                state->patients[left].id == state->patients[right].id) {
                return 0;
            }
        }
    }
    return 1;
}

static uint8_t storage_unique_doctor_ids(const struct hospital_storage_state *state) {
    for (uint32_t left = 0; left < STORAGE_MAX_DOCTORS; left++) {
        if (!state->doctors[left].in_use) {
            continue;
        }
        for (uint32_t right = left + 1; right < STORAGE_MAX_DOCTORS; right++) {
            if (state->doctors[right].in_use &&
                state->doctors[left].id == state->doctors[right].id) {
                return 0;
            }
        }
    }
    return 1;
}

static uint8_t storage_unique_record_ids(const struct hospital_storage_state *state) {
    for (uint32_t left = 0; left < STORAGE_MAX_RECORDS; left++) {
        if (!state->records[left].in_use) {
            continue;
        }
        for (uint32_t right = left + 1; right < STORAGE_MAX_RECORDS; right++) {
            if (state->records[right].in_use &&
                state->records[left].id == state->records[right].id) {
                return 0;
            }
        }
    }
    return 1;
}

static uint8_t storage_unique_appointment_ids(
    const struct hospital_storage_state *state) {
    for (uint32_t left = 0; left < STORAGE_MAX_APPOINTMENTS; left++) {
        if (!state->appointments[left].in_use) {
            continue;
        }
        for (uint32_t right = left + 1; right < STORAGE_MAX_APPOINTMENTS; right++) {
            if (state->appointments[right].in_use &&
                state->appointments[left].id == state->appointments[right].id) {
                return 0;
            }
        }
    }
    return 1;
}

static uint8_t storage_valid_counts(const struct hospital_storage_state *state) {
    uint32_t active_patients = 0;
    uint32_t active_records = 0;
    uint32_t active_doctors = 0;
    uint32_t active_appointments = 0;
    uint32_t active_assignments = 0;

    if (state->patient_count > STORAGE_MAX_PATIENTS ||
        state->record_count > STORAGE_MAX_RECORDS ||
        state->doctor_count > STORAGE_MAX_DOCTORS ||
        state->appointment_count > STORAGE_MAX_APPOINTMENTS ||
        state->assignment_count > STORAGE_MAX_ASSIGNMENTS) {
        return 0;
    }
    for (uint32_t index = 0; index < STORAGE_MAX_PATIENTS; index++) {
        if (state->patients[index].in_use > 1) {
            return 0;
        }
        active_patients += state->patients[index].in_use;
    }
    for (uint32_t index = 0; index < STORAGE_MAX_RECORDS; index++) {
        if (state->records[index].in_use > 1) {
            return 0;
        }
        active_records += state->records[index].in_use;
    }
    for (uint32_t index = 0; index < STORAGE_MAX_DOCTORS; index++) {
        if (state->doctors[index].in_use > 1) {
            return 0;
        }
        active_doctors += state->doctors[index].in_use;
    }
    for (uint32_t index = 0; index < STORAGE_MAX_APPOINTMENTS; index++) {
        if (state->appointments[index].in_use > 1) {
            return 0;
        }
        active_appointments += state->appointments[index].in_use;
    }
    for (uint32_t index = 0; index < STORAGE_MAX_ASSIGNMENTS; index++) {
        if (state->assignments[index].in_use > 1) {
            return 0;
        }
        active_assignments += state->assignments[index].in_use;
    }
    return active_patients == state->patient_count &&
           active_records == state->record_count &&
           active_doctors == state->doctor_count &&
           active_appointments == state->appointment_count &&
           active_assignments == state->assignment_count;
}

static uint8_t storage_valid_references(const struct hospital_storage_state *state) {
    if (!storage_unique_patient_ids(state) ||
        !storage_unique_doctor_ids(state) ||
        !storage_unique_record_ids(state) ||
        !storage_unique_appointment_ids(state)) {
        return 0;
    }

    for (uint32_t index = 0; index < STORAGE_MAX_PATIENTS; index++) {
        if (state->patients[index].in_use &&
            (state->patients[index].id == 0 ||
             state->patients[index].age < 1 ||
             state->patients[index].age > 120 ||
             !storage_string_valid(state->patients[index].name,
                                   STORAGE_PATIENT_NAME_CAPACITY))) {
            return 0;
        }
    }
    for (uint32_t index = 0; index < STORAGE_MAX_DOCTORS; index++) {
        if (state->doctors[index].in_use &&
            (state->doctors[index].id == 0 ||
             !storage_string_valid(state->doctors[index].specialty,
                                   STORAGE_DOCTOR_SPECIALTY_CAPACITY) ||
             !storage_string_valid(state->doctors[index].name,
                                   STORAGE_DOCTOR_NAME_CAPACITY))) {
            return 0;
        }
    }
    for (uint32_t index = 0; index < STORAGE_MAX_RECORDS; index++) {
        if (state->records[index].in_use &&
            (state->records[index].id == 0 ||
             !storage_patient_exists(state, state->records[index].patient_id) ||
             !storage_string_valid(state->records[index].diagnosis,
                                   STORAGE_RECORD_DIAGNOSIS_CAPACITY))) {
            return 0;
        }
    }
    for (uint32_t index = 0; index < STORAGE_MAX_APPOINTMENTS; index++) {
        if (state->appointments[index].in_use &&
            (state->appointments[index].id == 0 ||
             !storage_patient_exists(state, state->appointments[index].patient_id) ||
             !storage_doctor_exists(state, state->appointments[index].doctor_id))) {
            return 0;
        }
        for (uint32_t other = index + 1; other < STORAGE_MAX_APPOINTMENTS; other++) {
            if (state->appointments[index].in_use &&
                state->appointments[other].in_use &&
                state->appointments[index].slot == state->appointments[other].slot &&
                (state->appointments[index].doctor_id ==
                     state->appointments[other].doctor_id ||
                 state->appointments[index].patient_id ==
                     state->appointments[other].patient_id)) {
                return 0;
            }
        }
    }
    for (uint32_t index = 0; index < STORAGE_MAX_ASSIGNMENTS; index++) {
        if (state->assignments[index].in_use &&
            (state->assignments[index].patient_id == 0 ||
             state->assignments[index].doctor_id == 0 ||
             !storage_patient_exists(state, state->assignments[index].patient_id) ||
             !storage_doctor_exists(state, state->assignments[index].doctor_id))) {
            return 0;
        }
        for (uint32_t other = index + 1; other < STORAGE_MAX_ASSIGNMENTS; other++) {
            if (state->assignments[index].in_use &&
                state->assignments[other].in_use &&
                state->assignments[index].patient_id ==
                    state->assignments[other].patient_id) {
                return 0;
            }
        }
    }
    return 1;
}

static uint8_t storage_valid_banker(const struct hospital_storage_state *state) {
    uint64_t allocated[STORAGE_BANKER_RESOURCES] = {0, 0, 0};

    for (uint32_t process = 0; process < STORAGE_BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
            uint32_t allocation = state->banker_allocation[process][resource];
            uint32_t maximum = state->banker_maximum[process][resource];
            uint32_t need = state->banker_need[process][resource];
            if (allocation > maximum || need != maximum - allocation) {
                return 0;
            }
            allocated[resource] += allocation;
        }
    }
    for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
        if (allocated[resource] > state->banker_total[resource] ||
            state->banker_available[resource] !=
                state->banker_total[resource] - allocated[resource]) {
            return 0;
        }
    }
    return 1;
}

static uint8_t storage_state_valid(const struct hospital_storage_state *state) {
    return storage_valid_counts(state) && storage_valid_references(state) &&
           storage_valid_banker(state);
}

static int storage_encode_payload(const struct hospital_storage_state *state,
                                  uint8_t *payload, uint32_t payload_capacity,
                                  uint32_t *payload_length) {
    uint32_t offset = 0;
    if (state == (void *)0 || payload == (void *)0 ||
        payload_length == (void *)0 ||
        payload_capacity > STORAGE_PAYLOAD_CAPACITY ||
        !storage_state_valid(state)) {
        return STORAGE_ERR_VALIDATION;
    }

#define PUT_U32(value) do { \
    if (!storage_put_u32(payload, &offset, payload_capacity, (value))) { \
        return STORAGE_ERR_FORMAT; \
    } \
} while (0)
#define PUT_BYTES(value, length) do { \
    if (!storage_put_bytes(payload, &offset, payload_capacity, \
                           (value), (length))) { \
        return STORAGE_ERR_FORMAT; \
    } \
} while (0)
    PUT_U32(state->patient_count);
    PUT_U32(state->record_count);
    PUT_U32(state->doctor_count);
    PUT_U32(state->appointment_count);
    PUT_U32(state->assignment_count);
    for (uint32_t index = 0; index < STORAGE_MAX_PATIENTS; index++) {
        PUT_U32(state->patients[index].id);
        PUT_U32(state->patients[index].age);
        PUT_BYTES(state->patients[index].name, STORAGE_PATIENT_NAME_CAPACITY);
        PUT_BYTES(&state->patients[index].in_use, 1);
    }
    for (uint32_t index = 0; index < STORAGE_MAX_RECORDS; index++) {
        PUT_U32(state->records[index].id);
        PUT_U32(state->records[index].patient_id);
        PUT_BYTES(state->records[index].diagnosis,
                  STORAGE_RECORD_DIAGNOSIS_CAPACITY);
        PUT_BYTES(&state->records[index].in_use, 1);
    }
    for (uint32_t index = 0; index < STORAGE_MAX_DOCTORS; index++) {
        PUT_U32(state->doctors[index].id);
        PUT_BYTES(state->doctors[index].specialty,
                  STORAGE_DOCTOR_SPECIALTY_CAPACITY);
        PUT_BYTES(state->doctors[index].name, STORAGE_DOCTOR_NAME_CAPACITY);
        PUT_BYTES(&state->doctors[index].in_use, 1);
    }
    for (uint32_t index = 0; index < STORAGE_MAX_APPOINTMENTS; index++) {
        PUT_U32(state->appointments[index].id);
        PUT_U32(state->appointments[index].patient_id);
        PUT_U32(state->appointments[index].doctor_id);
        PUT_U32(state->appointments[index].slot);
        PUT_BYTES(&state->appointments[index].in_use, 1);
    }
    for (uint32_t index = 0; index < STORAGE_MAX_ASSIGNMENTS; index++) {
        PUT_U32(state->assignments[index].patient_id);
        PUT_U32(state->assignments[index].doctor_id);
        PUT_BYTES(&state->assignments[index].in_use, 1);
    }
    for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
        PUT_U32(state->banker_total[resource]);
    }
    for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
        PUT_U32(state->banker_available[resource]);
    }
    for (uint32_t process = 0; process < STORAGE_BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
            PUT_U32(state->banker_allocation[process][resource]);
        }
    }
    for (uint32_t process = 0; process < STORAGE_BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
            PUT_U32(state->banker_maximum[process][resource]);
        }
    }
    for (uint32_t process = 0; process < STORAGE_BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
            PUT_U32(state->banker_need[process][resource]);
        }
    }
#undef PUT_U32
#undef PUT_BYTES

    if (offset != STORAGE_SERIALIZED_PAYLOAD_SIZE) {
        return STORAGE_ERR_FORMAT;
    }
    *payload_length = offset;
    return STORAGE_OK;
}

static int storage_decode_payload(const uint8_t *payload, uint32_t payload_length,
                                  struct hospital_storage_state *state) {
    uint32_t offset = 0;
    if (payload_length != STORAGE_SERIALIZED_PAYLOAD_SIZE) {
        return STORAGE_ERR_FORMAT;
    }

#define GET_U32(destination) do { \
    if (!storage_get_u32(payload, &offset, payload_length, &(destination))) { \
        return STORAGE_ERR_FORMAT; \
    } \
} while (0)
#define GET_BYTES(destination, length) do { \
    if (!storage_get_bytes(payload, &offset, payload_length, (destination), \
                           (length))) { \
        return STORAGE_ERR_FORMAT; \
    } \
} while (0)
    GET_U32(state->patient_count);
    GET_U32(state->record_count);
    GET_U32(state->doctor_count);
    GET_U32(state->appointment_count);
    GET_U32(state->assignment_count);
    for (uint32_t index = 0; index < STORAGE_MAX_PATIENTS; index++) {
        GET_U32(state->patients[index].id);
        GET_U32(state->patients[index].age);
        GET_BYTES(state->patients[index].name, STORAGE_PATIENT_NAME_CAPACITY);
        GET_BYTES(&state->patients[index].in_use, 1);
    }
    for (uint32_t index = 0; index < STORAGE_MAX_RECORDS; index++) {
        GET_U32(state->records[index].id);
        GET_U32(state->records[index].patient_id);
        GET_BYTES(state->records[index].diagnosis,
                  STORAGE_RECORD_DIAGNOSIS_CAPACITY);
        GET_BYTES(&state->records[index].in_use, 1);
    }
    for (uint32_t index = 0; index < STORAGE_MAX_DOCTORS; index++) {
        GET_U32(state->doctors[index].id);
        GET_BYTES(state->doctors[index].specialty,
                  STORAGE_DOCTOR_SPECIALTY_CAPACITY);
        GET_BYTES(state->doctors[index].name, STORAGE_DOCTOR_NAME_CAPACITY);
        GET_BYTES(&state->doctors[index].in_use, 1);
    }
    for (uint32_t index = 0; index < STORAGE_MAX_APPOINTMENTS; index++) {
        GET_U32(state->appointments[index].id);
        GET_U32(state->appointments[index].patient_id);
        GET_U32(state->appointments[index].doctor_id);
        GET_U32(state->appointments[index].slot);
        GET_BYTES(&state->appointments[index].in_use, 1);
    }
    for (uint32_t index = 0; index < STORAGE_MAX_ASSIGNMENTS; index++) {
        GET_U32(state->assignments[index].patient_id);
        GET_U32(state->assignments[index].doctor_id);
        GET_BYTES(&state->assignments[index].in_use, 1);
    }
    for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
        GET_U32(state->banker_total[resource]);
    }
    for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
        GET_U32(state->banker_available[resource]);
    }
    for (uint32_t process = 0; process < STORAGE_BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
            GET_U32(state->banker_allocation[process][resource]);
        }
    }
    for (uint32_t process = 0; process < STORAGE_BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
            GET_U32(state->banker_maximum[process][resource]);
        }
    }
    for (uint32_t process = 0; process < STORAGE_BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < STORAGE_BANKER_RESOURCES; resource++) {
            GET_U32(state->banker_need[process][resource]);
        }
    }
#undef GET_U32
#undef GET_BYTES

    if (offset != payload_length || !storage_state_valid(state)) {
        return STORAGE_ERR_VALIDATION;
    }
    return STORAGE_OK;
}

static uint32_t storage_checksum(const uint8_t *payload, uint32_t length) {
    uint32_t checksum = 2166136261u;
    for (uint32_t index = 0; index < length; index++) {
        checksum ^= payload[index];
        checksum *= 16777619u;
    }
    return checksum;
}

static int storage_put_header(uint8_t *buffer, uint32_t payload_length,
                              uint32_t generation, uint32_t checksum) {
    uint32_t offset = 0;
    if (!storage_put_u32(buffer, &offset, STORAGE_HEADER_SIZE, STORAGE_MAGIC) ||
        !storage_put_u32(buffer, &offset, STORAGE_HEADER_SIZE,
                         STORAGE_FORMAT_VERSION) ||
        !storage_put_u32(buffer, &offset, STORAGE_HEADER_SIZE,
                         STORAGE_HEADER_SIZE) ||
        !storage_put_u32(buffer, &offset, STORAGE_HEADER_SIZE, payload_length) ||
        !storage_put_u32(buffer, &offset, STORAGE_HEADER_SIZE, generation) ||
        !storage_put_u32(buffer, &offset, STORAGE_HEADER_SIZE, checksum) ||
        !storage_put_u32(buffer, &offset, STORAGE_HEADER_SIZE, 0) ||
        !storage_put_u32(buffer, &offset, STORAGE_HEADER_SIZE, 0)) {
        return STORAGE_ERR_FORMAT;
    }
    return offset == STORAGE_HEADER_SIZE ? STORAGE_OK : STORAGE_ERR_FORMAT;
}

static uint32_t storage_header_u32(const uint8_t *buffer, uint32_t field) {
    uint32_t offset = field * 4u;
    return (uint32_t)buffer[offset + 0] |
           ((uint32_t)buffer[offset + 1] << 8) |
           ((uint32_t)buffer[offset + 2] << 16) |
           ((uint32_t)buffer[offset + 3] << 24);
}

static int storage_read_slot(uint32_t slot) {
    uint32_t lba;
    if (storage_slot_lba(slot, &lba) != STORAGE_OK) {
        return STORAGE_ERR_ARGUMENT;
    }
    for (uint32_t sector = 0; sector < STORAGE_SLOT_SECTORS; sector++) {
        if (ata_read_sector(0, lba + sector,
                            &storage_slot_buffer[sector * ATA_SECTOR_SIZE]) != 0) {
            return STORAGE_ERR_IO;
        }
    }
    return STORAGE_OK;
}

static int storage_write_slot(uint32_t slot) {
    uint32_t lba;
    if (storage_slot_lba(slot, &lba) != STORAGE_OK) {
        return STORAGE_ERR_ARGUMENT;
    }
    for (uint32_t sector = 0; sector < STORAGE_SLOT_SECTORS; sector++) {
        if (ata_write_sector(0, lba + sector,
                             &storage_slot_buffer[sector * ATA_SECTOR_SIZE]) != 0) {
            return STORAGE_ERR_IO;
        }
    }
    return STORAGE_OK;
}

static int storage_decode_slot(struct hospital_storage_state *state,
                               uint32_t *generation) {
    uint32_t payload_length = storage_header_u32(storage_slot_buffer, 3);
    uint32_t stored_generation = storage_header_u32(storage_slot_buffer, 4);
    uint32_t stored_checksum = storage_header_u32(storage_slot_buffer, 5);

    if (storage_header_u32(storage_slot_buffer, 0) != STORAGE_MAGIC ||
        storage_header_u32(storage_slot_buffer, 1) != STORAGE_FORMAT_VERSION ||
        storage_header_u32(storage_slot_buffer, 2) != STORAGE_HEADER_SIZE ||
        payload_length != STORAGE_SERIALIZED_PAYLOAD_SIZE ||
        payload_length > STORAGE_PAYLOAD_CAPACITY || stored_generation == 0 ||
        storage_header_u32(storage_slot_buffer, 6) != 0 ||
        storage_header_u32(storage_slot_buffer, 7) != 0 ||
        stored_checksum != storage_checksum(storage_slot_buffer + STORAGE_HEADER_SIZE,
                                            payload_length)) {
        return STORAGE_ERR_FORMAT;
    }
    if (storage_decode_payload(storage_slot_buffer + STORAGE_HEADER_SIZE,
                               payload_length, state) != STORAGE_OK) {
        return STORAGE_ERR_VALIDATION;
    }
    *generation = stored_generation;
    return STORAGE_OK;
}

/* Modular ordering: a nonzero distance below 2^31 means candidate is newer.
 * A distance of exactly 2^31 is ambiguous and is never selected as newer. */
static uint8_t storage_generation_newer(uint32_t candidate,
                                        uint32_t current) {
    uint32_t distance = candidate - current;
    return distance != 0 && distance < 0x80000000u;
}

static int storage_find_latest(void) {
    uint8_t found = 0;
    uint8_t had_io_error = 0;
    uint32_t best_generation = 0;
    int32_t best_slot = -1;

    for (uint32_t slot = 0; slot < STORAGE_SLOT_COUNT; slot++) {
        uint32_t generation;
        int result = storage_read_slot(slot);
        if (result != STORAGE_OK) {
            had_io_error = 1;
            continue;
        }
        if (storage_decode_slot(&storage_candidate, &generation) != STORAGE_OK) {
            continue;
        }
        if (!found || storage_generation_newer(generation, best_generation)) {
            found = 1;
            best_generation = generation;
            best_slot = (int32_t)slot;
            storage_best = storage_candidate;
        }
    }

    if (!found) {
        storage_active_slot = -1;
        storage_current_generation = 0;
        return had_io_error ? STORAGE_ERR_IO : STORAGE_NO_SNAPSHOT;
    }
    storage_active_slot = best_slot;
    storage_current_generation = best_generation;
    return STORAGE_OK;
}

int storage_initialize(void) {
    const struct ata_drive_info *drive;

    storage_initialized = 0;
    storage_active_slot = -1;
    storage_current_generation = 0;
    if (!storage_layout_valid()) {
        return STORAGE_ERR_LAYOUT;
    }

    ata_initialize();
    drive = ata_drive_info(0);
    if (drive == (void *)0 || drive->sector_count != STORAGE_TEST_DISK_SECTORS) {
        return STORAGE_ERR_LAYOUT;
    }
    storage_initialized = 1;
    return STORAGE_OK;
}

int storage_load(struct hospital_storage_state *state, uint32_t *generation) {
    int result;

    if (state == (void *)0 || generation == (void *)0) {
        return STORAGE_ERR_ARGUMENT;
    }
    if (!storage_initialized) {
        return STORAGE_ERR_NO_INITIALIZATION;
    }
    result = storage_find_latest();
    if (result != STORAGE_OK) {
        return result;
    }
    *state = storage_best;
    *generation = storage_current_generation;
    return STORAGE_OK;
}

int storage_save(const struct hospital_storage_state *state,
                 uint32_t *generation) {
    uint32_t payload_length;
    uint32_t next_generation;
    uint32_t target_slot;
    int result;

    if (state == (void *)0 || generation == (void *)0) {
        return STORAGE_ERR_ARGUMENT;
    }
    if (!storage_initialized) {
        return STORAGE_ERR_NO_INITIALIZATION;
    }
    if (storage_state_valid(state) == 0) {
        return STORAGE_ERR_VALIDATION;
    }
    if (storage_active_slot < 0) {
        result = storage_find_latest();
        if (result != STORAGE_OK && result != STORAGE_NO_SNAPSHOT) {
            return result;
        }
    }

    target_slot = storage_active_slot < 0 ? 0u :
                  (uint32_t)(1 - storage_active_slot);
    next_generation = storage_current_generation == 0xffffffffu ? 1u :
                      storage_current_generation + 1u;

    for (uint32_t index = 0; index < STORAGE_SLOT_BYTES; index++) {
        storage_slot_buffer[index] = 0;
    }
    result = storage_encode_payload(state, storage_expected_payload,
                                    STORAGE_SERIALIZED_PAYLOAD_SIZE,
                                    &payload_length);
    if (result != STORAGE_OK) {
        return result;
    }
    for (uint32_t index = 0; index < payload_length; index++) {
        storage_slot_buffer[STORAGE_HEADER_SIZE + index] =
            storage_expected_payload[index];
    }
    result = storage_put_header(
        storage_slot_buffer, payload_length, next_generation,
        storage_checksum(storage_expected_payload, payload_length));
    if (result != STORAGE_OK) {
        return result;
    }

    result = storage_write_slot(target_slot);
    if (result != STORAGE_OK) {
        return result;
    }
    result = storage_read_slot(target_slot);
    if (result != STORAGE_OK) {
        return STORAGE_ERR_IO;
    }
    {
        uint32_t verified_generation;
        result = storage_decode_slot(&storage_candidate, &verified_generation);
        if (result != STORAGE_OK) {
            return result;
        }
        if (verified_generation != next_generation) {
            return STORAGE_ERR_FORMAT;
        }
        for (uint32_t index = 0; index < payload_length; index++) {
            if (storage_slot_buffer[STORAGE_HEADER_SIZE + index] !=
                storage_expected_payload[index]) {
                return STORAGE_ERR_VERIFY;
            }
        }
    }

    storage_active_slot = (int32_t)target_slot;
    storage_current_generation = next_generation;
    *generation = next_generation;
    return STORAGE_OK;
}
