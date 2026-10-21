#include <stdint.h>

#include "ata.h"
#include "limine.h"
#include "storage.h"

/* Limine scans these aligned objects for boot protocol requests. */
__attribute__((used, section(".limine_requests_start"), aligned(8)))
static volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile struct limine_framebuffer_request framebuffer_request = {
    LIMINE_FRAMEBUFFER_REQUEST_ID,
    0,
    (void *)0,
};

__attribute__((used, section(".limine_requests_end"), aligned(8)))
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

#define FRAMEBUFFER_BACKGROUND 0x00102030u
#define FRAMEBUFFER_FOREGROUND 0x00ffffffu
#define CONSOLE_MARGIN 32u
#define CONSOLE_SCALE 4u
#define GLYPH_WIDTH 5u
#define GLYPH_HEIGHT 7u
#define GLYPH_ADVANCE_X (6u * CONSOLE_SCALE)
#define GLYPH_ADVANCE_Y (8u * CONSOLE_SCALE)
#define COMMAND_CAPACITY 128u
#define CONSOLE_HISTORY_LINES 256u
#define CONSOLE_HISTORY_COLUMNS 128u
#define MAX_PATIENTS 32u
#define PATIENT_NAME_CAPACITY 32u
#define MAX_RECORDS 32u
#define RECORD_DIAGNOSIS_CAPACITY 64u
#define MAX_DOCTORS 16u
#define DOCTOR_NAME_CAPACITY 32u
#define DOCTOR_SPECIALTY_CAPACITY 32u
#define MAX_APPOINTMENTS 32u
#define MAX_ASSIGNMENTS 32u
#define BANKER_PROCESSES 5u
#define BANKER_RESOURCES 3u
#define ATA_TEST_DRIVE_INDEX 0u
#define ATA_TEST_DISK_SECTORS 131072u
#define ATA_TEST_LBA (ATA_TEST_DISK_SECTORS - 2u)

struct patient {
    uint32_t id;
    uint32_t age;
    char name[PATIENT_NAME_CAPACITY];
    uint8_t in_use;
};

struct medical_record {
    uint32_t id;
    uint32_t patient_id;
    char diagnosis[RECORD_DIAGNOSIS_CAPACITY];
    uint8_t in_use;
};

struct doctor {
    uint32_t id;
    char specialty[DOCTOR_SPECIALTY_CAPACITY];
    char name[DOCTOR_NAME_CAPACITY];
    uint8_t in_use;
};

struct appointment {
    uint32_t id;
    uint32_t patient_id;
    uint32_t doctor_id;
    uint32_t slot;
    uint8_t in_use;
};

struct doctor_assignment {
    uint32_t patient_id;
    uint32_t doctor_id;
    uint8_t in_use;
};

static struct limine_framebuffer *active_framebuffer;
static uint64_t console_column;
static uint64_t console_columns;
static uint64_t console_rows;
static char console_history[CONSOLE_HISTORY_LINES][CONSOLE_HISTORY_COLUMNS];
static uint32_t console_history_count;
static uint32_t console_history_view_offset;

static char command_buffer[COMMAND_CAPACITY];
static uint64_t command_length;

static struct patient patients[MAX_PATIENTS];
static uint32_t patient_count;
static struct medical_record records[MAX_RECORDS];
static uint32_t record_count;
static struct doctor doctors[MAX_DOCTORS];
static uint32_t doctor_count;
static struct appointment appointments[MAX_APPOINTMENTS];
static uint32_t appointment_count;
static struct doctor_assignment assignments[MAX_ASSIGNMENTS];
static uint32_t assignment_count;
static uint8_t ata_test_original[ATA_SECTOR_SIZE];
static uint8_t ata_test_pattern[ATA_SECTOR_SIZE];
static uint8_t ata_test_readback[ATA_SECTOR_SIZE];

static struct hospital_storage_state storage_transfer_state;
static uint8_t storage_available;
static uint8_t storage_snapshot_loaded;
static uint8_t storage_snapshot_saved;
static uint32_t storage_generation;
static int storage_last_result;
static uint8_t mutation_succeeded;

static void console_write_number(uint32_t value);

/* Resource order: ICU beds, ventilators, patient monitors. */
static uint32_t banker_total[BANKER_RESOURCES] = {10, 5, 7};
static uint32_t banker_available[BANKER_RESOURCES] = {3, 3, 2};
static uint32_t banker_allocation[BANKER_PROCESSES][BANKER_RESOURCES] = {
    {0, 1, 0},
    {2, 0, 0},
    {3, 0, 2},
    {2, 1, 1},
    {0, 0, 2},
};
static uint32_t banker_maximum[BANKER_PROCESSES][BANKER_RESOURCES] = {
    {7, 5, 3},
    {3, 2, 2},
    {9, 0, 2},
    {2, 2, 2},
    {4, 3, 3},
};
static uint32_t banker_need[BANKER_PROCESSES][BANKER_RESOURCES] = {
    {7, 4, 3},
    {1, 2, 2},
    {6, 0, 0},
    {0, 1, 1},
    {4, 3, 1},
};

static uint8_t keyboard_shift;
static uint8_t keyboard_extended;

static struct limine_framebuffer *framebuffer(void) {
    if (framebuffer_request.response == (void *)0 ||
        framebuffer_request.response->framebuffer_count == 0) {
        return (void *)0;
    }

    return framebuffer_request.response->framebuffers[0];
}

static void put_pixel(struct limine_framebuffer *fb,
                      uint64_t x, uint64_t y, uint32_t colour) {
    if (x >= fb->width || y >= fb->height || fb->bpp != 32) {
        return;
    }

    uint8_t *row = (uint8_t *)fb->address + y * fb->pitch;
    ((uint32_t *)row)[x] = colour;
}

static void fill_rect(uint64_t x, uint64_t y, uint64_t width,
                      uint64_t height, uint32_t colour) {
    for (uint64_t row = 0; row < height; row++) {
        for (uint64_t column = 0; column < width; column++) {
            put_pixel(active_framebuffer, x + column, y + row, colour);
        }
    }
}

/* A compact 5x7 bitmap font for the CLI's lowercase text. */
static const uint8_t lowercase_font[26][7] = {
    {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f}, /* a */
    {0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x1e}, /* b */
    {0x00, 0x00, 0x0f, 0x10, 0x10, 0x10, 0x0f}, /* c */
    {0x01, 0x01, 0x0f, 0x11, 0x11, 0x13, 0x0d}, /* d */
    {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0f}, /* e */
    {0x06, 0x09, 0x08, 0x1e, 0x08, 0x08, 0x08}, /* f */
    {0x00, 0x00, 0x0f, 0x11, 0x0f, 0x01, 0x1e}, /* g */
    {0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x11}, /* h */
    {0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e}, /* i */
    {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0c}, /* j */
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, /* k */
    {0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, /* l */
    {0x00, 0x00, 0x1a, 0x15, 0x15, 0x11, 0x11}, /* m */
    {0x00, 0x00, 0x1e, 0x11, 0x11, 0x11, 0x11}, /* n */
    {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e}, /* o */
    {0x00, 0x00, 0x1e, 0x11, 0x1e, 0x10, 0x10}, /* p */
    {0x00, 0x00, 0x0f, 0x11, 0x0f, 0x01, 0x01}, /* q */
    {0x00, 0x00, 0x17, 0x18, 0x10, 0x10, 0x10}, /* r */
    {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e}, /* s */
    {0x08, 0x08, 0x1e, 0x08, 0x08, 0x09, 0x06}, /* t */
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d}, /* u */
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04}, /* v */
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a}, /* w */
    {0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11}, /* x */
    {0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e}, /* y */
    {0x00, 0x00, 0x1f, 0x02, 0x04, 0x08, 0x1f}, /* z */
};

static uint8_t glyph_row(char character, uint64_t row) {
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t uppercase_h[7] = {0x11, 0x11, 0x1f, 0x11, 0x11, 0x11, 0x11};
    static const uint8_t uppercase_o[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t uppercase_s[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    static const uint8_t zero[7] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
    static const uint8_t one[7] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e};
    static const uint8_t two[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
    static const uint8_t three[7] = {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e};
    static const uint8_t four[7] = {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02};
    static const uint8_t five[7] = {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e};
    static const uint8_t six[7] = {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e};
    static const uint8_t seven[7] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static const uint8_t eight[7] = {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e};
    static const uint8_t nine[7] = {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x1c};
    static const uint8_t greater_than[7] = {0x00, 0x10, 0x08, 0x04, 0x08, 0x10, 0x00};
    static const uint8_t period[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c};
    static const uint8_t comma[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x08};
    static const uint8_t hyphen[7] = {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};

    if (row >= 7) {
        return 0;
    }
    if (character >= 'a' && character <= 'z') {
        return lowercase_font[(uint64_t)(character - 'a')][row];
    }

    switch (character) {
        case 'H': return uppercase_h[row];
        case 'O': return uppercase_o[row];
        case 'S': return uppercase_s[row];
        case '0': return zero[row];
        case '1': return one[row];
        case '2': return two[row];
        case '3': return three[row];
        case '4': return four[row];
        case '5': return five[row];
        case '6': return six[row];
        case '7': return seven[row];
        case '8': return eight[row];
        case '9': return nine[row];
        case '>': return greater_than[row];
        case '.': return period[row];
        case ',': return comma[row];
        case '-': return hyphen[row];
        case ' ': return blank[row];
        default:
            if (character >= 'A' && character <= 'Z') {
                return lowercase_font[(uint64_t)(character - 'A')][row];
            }
            return blank[row];
    }
}

static void draw_glyph(char character, uint64_t x, uint64_t y) {
    for (uint64_t row = 0; row < GLYPH_HEIGHT; row++) {
        uint8_t bits = glyph_row(character, row);
        for (uint64_t column = 0; column < GLYPH_WIDTH; column++) {
            if ((bits & (1u << (4 - column))) != 0) {
                fill_rect(x + column * CONSOLE_SCALE,
                          y + row * CONSOLE_SCALE,
                          CONSOLE_SCALE, CONSOLE_SCALE,
                          FRAMEBUFFER_FOREGROUND);
            }
        }
    }
}

static void console_fill_background(void) {
    for (uint64_t y = 0; y < active_framebuffer->height; y++) {
        for (uint64_t x = 0; x < active_framebuffer->width; x++) {
            put_pixel(active_framebuffer, x, y, FRAMEBUFFER_BACKGROUND);
        }
    }
}

static void console_clear_history_line(uint32_t line) {
    for (uint32_t column = 0; column < CONSOLE_HISTORY_COLUMNS; column++) {
        console_history[line][column] = '\0';
    }
}

static void console_render(void) {
    console_fill_background();

    if (console_history_count == 0) {
        return;
    }

    uint32_t last_line = console_history_count - 1;
    uint32_t max_offset = 0;
    if (console_history_count > console_rows) {
        max_offset = console_history_count - (uint32_t)console_rows;
    }
    if (console_history_view_offset > max_offset) {
        console_history_view_offset = max_offset;
    }

    uint32_t end_line = last_line - console_history_view_offset;
    uint32_t first_line = 0;
    if (end_line + 1 > console_rows) {
        first_line = end_line + 1 - (uint32_t)console_rows;
    }

    uint32_t screen_row = 0;
    for (uint32_t line = first_line; line <= end_line; line++) {
        for (uint32_t column = 0;
             column < console_columns && column < CONSOLE_HISTORY_COLUMNS;
             column++) {
            char character = console_history[line][column];
            if (character == '\0') {
                break;
            }
            draw_glyph(character,
                       CONSOLE_MARGIN + column * GLYPH_ADVANCE_X,
                       CONSOLE_MARGIN + screen_row * GLYPH_ADVANCE_Y);
        }
        screen_row++;
        if (screen_row >= console_rows) {
            break;
        }
    }
}

static void console_clear(void) {
    console_history_count = 1;
    console_history_view_offset = 0;
    console_column = 0;
    console_clear_history_line(0);
    console_render();
}

static void console_newline(void) {
    console_column = 0;

    if (console_history_count < CONSOLE_HISTORY_LINES) {
        console_clear_history_line(console_history_count);
        console_history_count++;
    } else {
        for (uint32_t line = 1; line < CONSOLE_HISTORY_LINES; line++) {
            for (uint32_t column = 0; column < CONSOLE_HISTORY_COLUMNS; column++) {
                console_history[line - 1][column] = console_history[line][column];
            }
        }
        console_clear_history_line(CONSOLE_HISTORY_LINES - 1);
    }

    console_history_view_offset = 0;
    console_render();
}

static void console_backspace(void) {
    if (console_column == 0 || console_history_count == 0) {
        return;
    }

    console_column--;
    console_history[console_history_count - 1][console_column] = '\0';
    console_history_view_offset = 0;
    console_render();
}

static void console_putc(char character) {
    if (character == '\n') {
        console_newline();
        return;
    }

    if (console_history_count == 0) {
        console_clear();
    }
    if (console_column >= console_columns ||
        console_column >= CONSOLE_HISTORY_COLUMNS - 1) {
        console_newline();
    }

    console_history[console_history_count - 1][console_column] = character;
    console_column++;
    console_history_view_offset = 0;
    console_render();
}

static void console_page_up(void) {
    uint32_t page = (uint32_t)console_rows;
    uint32_t max_offset = 0;

    if (console_history_count > console_rows) {
        max_offset = console_history_count - (uint32_t)console_rows;
    }
    if (console_history_view_offset + page > max_offset) {
        console_history_view_offset = max_offset;
    } else {
        console_history_view_offset += page;
    }
    console_render();
}

static void console_page_down(void) {
    uint32_t page = (uint32_t)console_rows;

    if (console_history_view_offset > page) {
        console_history_view_offset -= page;
    } else {
        console_history_view_offset = 0;
    }
    console_render();
}

static void console_write(const char *text) {
    for (uint64_t index = 0; text[index] != '\0'; index++) {
        console_putc(text[index]);
    }
}

static void console_prompt(void) {
    console_write("HospitalOS> ");
}

static char ascii_lower(char character) {
    if (character >= 'A' && character <= 'Z') {
        return (char)(character + ('a' - 'A'));
    }
    return character;
}

static uint8_t strings_equal(const char *left, const char *right) {
    uint64_t index = 0;
    while (left[index] != '\0' || right[index] != '\0') {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return 0;
        }
        index++;
    }
    return 1;
}

static const char *command_arguments(const char *command_name) {
    uint64_t index = 0;

    while (command_name[index] != '\0') {
        if (ascii_lower(command_buffer[index]) !=
            ascii_lower(command_name[index])) {
            return (void *)0;
        }
        index++;
    }

    if (command_buffer[index] == '\0') {
        return command_buffer + index;
    }
    if (command_buffer[index] == ' ') {
        return command_buffer + index + 1;
    }
    return (void *)0;
}

static void skip_spaces(const char **cursor) {
    while (**cursor == ' ') {
        (*cursor)++;
    }
}

static uint8_t parse_number_token(const char **cursor, uint32_t *value) {
    uint64_t number = 0;
    uint8_t digits = 0;

    skip_spaces(cursor);
    while (**cursor >= '0' && **cursor <= '9') {
        uint64_t digit = (uint64_t)(**cursor - '0');
        if (number > (0xffffffffu - digit) / 10u) {
            return 0;
        }
        number = number * 10u + digit;
        digits = 1;
        (*cursor)++;
    }

    if (!digits || (**cursor != '\0' && **cursor != ' ')) {
        return 0;
    }

    *value = (uint32_t)number;
    return 1;
}

static uint32_t find_patient(uint32_t id) {
    for (uint32_t index = 0; index < patient_count; index++) {
        if (patients[index].in_use && patients[index].id == id) {
            return index;
        }
    }
    return MAX_PATIENTS;
}

static uint32_t find_record(uint32_t id) {
    for (uint32_t index = 0; index < record_count; index++) {
        if (records[index].in_use && records[index].id == id) {
            return index;
        }
    }
    return MAX_RECORDS;
}

static uint32_t find_doctor(uint32_t id) {
    for (uint32_t index = 0; index < doctor_count; index++) {
        if (doctors[index].in_use && doctors[index].id == id) {
            return index;
        }
    }
    return MAX_DOCTORS;
}

static uint32_t find_appointment(uint32_t id) {
    for (uint32_t index = 0; index < appointment_count; index++) {
        if (appointments[index].in_use && appointments[index].id == id) {
            return index;
        }
    }
    return MAX_APPOINTMENTS;
}

static uint32_t find_assignment(uint32_t patient_id) {
    for (uint32_t index = 0; index < MAX_ASSIGNMENTS; index++) {
        if (assignments[index].in_use &&
            assignments[index].patient_id == patient_id) {
            return index;
        }
    }
    return MAX_ASSIGNMENTS;
}

/* Copy only the explicit persistent application state; transient kernel state
 * such as the framebuffer, keyboard, console, and pointers is excluded. */
static void live_state_to_storage(struct hospital_storage_state *state) {
    state->patient_count = patient_count;
    for (uint32_t index = 0; index < MAX_PATIENTS; index++) {
        state->patients[index].id = patients[index].id;
        state->patients[index].age = patients[index].age;
        state->patients[index].in_use = patients[index].in_use;
        for (uint32_t character = 0; character < PATIENT_NAME_CAPACITY; character++) {
            state->patients[index].name[character] = patients[index].name[character];
        }
    }

    state->record_count = record_count;
    for (uint32_t index = 0; index < MAX_RECORDS; index++) {
        state->records[index].id = records[index].id;
        state->records[index].patient_id = records[index].patient_id;
        state->records[index].in_use = records[index].in_use;
        for (uint32_t character = 0; character < RECORD_DIAGNOSIS_CAPACITY; character++) {
            state->records[index].diagnosis[character] = records[index].diagnosis[character];
        }
    }

    state->doctor_count = doctor_count;
    for (uint32_t index = 0; index < MAX_DOCTORS; index++) {
        state->doctors[index].id = doctors[index].id;
        state->doctors[index].in_use = doctors[index].in_use;
        for (uint32_t character = 0; character < DOCTOR_SPECIALTY_CAPACITY; character++) {
            state->doctors[index].specialty[character] = doctors[index].specialty[character];
            state->doctors[index].name[character] = doctors[index].name[character];
        }
    }

    state->appointment_count = appointment_count;
    for (uint32_t index = 0; index < MAX_APPOINTMENTS; index++) {
        state->appointments[index].id = appointments[index].id;
        state->appointments[index].patient_id = appointments[index].patient_id;
        state->appointments[index].doctor_id = appointments[index].doctor_id;
        state->appointments[index].slot = appointments[index].slot;
        state->appointments[index].in_use = appointments[index].in_use;
    }

    state->assignment_count = assignment_count;
    for (uint32_t index = 0; index < MAX_ASSIGNMENTS; index++) {
        state->assignments[index].patient_id = assignments[index].patient_id;
        state->assignments[index].doctor_id = assignments[index].doctor_id;
        state->assignments[index].in_use = assignments[index].in_use;
    }

    for (uint32_t resource = 0; resource < BANKER_RESOURCES; resource++) {
        state->banker_total[resource] = banker_total[resource];
        state->banker_available[resource] = banker_available[resource];
    }
    for (uint32_t process = 0; process < BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < BANKER_RESOURCES; resource++) {
            state->banker_allocation[process][resource] = banker_allocation[process][resource];
            state->banker_maximum[process][resource] = banker_maximum[process][resource];
            state->banker_need[process][resource] = banker_need[process][resource];
        }
    }
}

static void storage_state_to_live(const struct hospital_storage_state *state) {
    patient_count = state->patient_count;
    for (uint32_t index = 0; index < MAX_PATIENTS; index++) {
        patients[index].id = state->patients[index].id;
        patients[index].age = state->patients[index].age;
        patients[index].in_use = state->patients[index].in_use;
        for (uint32_t character = 0; character < PATIENT_NAME_CAPACITY; character++) {
            patients[index].name[character] = state->patients[index].name[character];
        }
    }

    record_count = state->record_count;
    for (uint32_t index = 0; index < MAX_RECORDS; index++) {
        records[index].id = state->records[index].id;
        records[index].patient_id = state->records[index].patient_id;
        records[index].in_use = state->records[index].in_use;
        for (uint32_t character = 0; character < RECORD_DIAGNOSIS_CAPACITY; character++) {
            records[index].diagnosis[character] = state->records[index].diagnosis[character];
        }
    }

    doctor_count = state->doctor_count;
    for (uint32_t index = 0; index < MAX_DOCTORS; index++) {
        doctors[index].id = state->doctors[index].id;
        doctors[index].in_use = state->doctors[index].in_use;
        for (uint32_t character = 0; character < DOCTOR_SPECIALTY_CAPACITY; character++) {
            doctors[index].specialty[character] = state->doctors[index].specialty[character];
            doctors[index].name[character] = state->doctors[index].name[character];
        }
    }

    appointment_count = state->appointment_count;
    for (uint32_t index = 0; index < MAX_APPOINTMENTS; index++) {
        appointments[index].id = state->appointments[index].id;
        appointments[index].patient_id = state->appointments[index].patient_id;
        appointments[index].doctor_id = state->appointments[index].doctor_id;
        appointments[index].slot = state->appointments[index].slot;
        appointments[index].in_use = state->appointments[index].in_use;
    }

    assignment_count = state->assignment_count;
    for (uint32_t index = 0; index < MAX_ASSIGNMENTS; index++) {
        assignments[index].patient_id = state->assignments[index].patient_id;
        assignments[index].doctor_id = state->assignments[index].doctor_id;
        assignments[index].in_use = state->assignments[index].in_use;
    }

    for (uint32_t resource = 0; resource < BANKER_RESOURCES; resource++) {
        banker_total[resource] = state->banker_total[resource];
        banker_available[resource] = state->banker_available[resource];
    }
    for (uint32_t process = 0; process < BANKER_PROCESSES; process++) {
        for (uint32_t resource = 0; resource < BANKER_RESOURCES; resource++) {
            banker_allocation[process][resource] = state->banker_allocation[process][resource];
            banker_maximum[process][resource] = state->banker_maximum[process][resource];
            banker_need[process][resource] = state->banker_need[process][resource];
        }
    }
}

static void storage_save_live_state(void) {
    int result;

    if (!storage_available) {
        console_write("warning: in-memory change succeeded; storage unavailable\n");
        return;
    }

    live_state_to_storage(&storage_transfer_state);
    result = storage_save(&storage_transfer_state, &storage_generation);
    storage_last_result = result;
    if (result != STORAGE_OK) {
        console_write("warning: in-memory change succeeded; storage save failed (error ");
        console_write_number((uint32_t)(-result));
        console_write(")\n");
        return;
    }

    storage_snapshot_saved = 1;
}

static void storage_status(void) {
    if (!storage_available) {
        console_write("storage unavailable; using in-memory data\n");
        return;
    }

    console_write("storage available\n");
    console_write(storage_snapshot_loaded ? "snapshot loaded\n" : "no snapshot loaded\n");
    console_write(storage_snapshot_saved ? "snapshot saved\n" : "no snapshot saved\n");
    console_write("generation ");
    console_write_number(storage_generation);
    console_putc('\n');
}

static void storage_save_command(void) {
    if (!storage_available) {
        console_write("storage unavailable; save skipped\n");
        return;
    }

    live_state_to_storage(&storage_transfer_state);
    storage_last_result = storage_save(&storage_transfer_state, &storage_generation);
    if (storage_last_result != STORAGE_OK) {
        console_write("storage save failed (error ");
        console_write_number((uint32_t)(-storage_last_result));
        console_write(")\n");
        return;
    }
    storage_snapshot_saved = 1;
    console_write("snapshot saved generation ");
    console_write_number(storage_generation);
    console_putc('\n');
}

static void storage_load_command(void) {
    int result;
    uint32_t generation;

    if (!storage_available) {
        console_write("storage unavailable; load skipped\n");
        return;
    }

    result = storage_load(&storage_transfer_state, &generation);
    storage_last_result = result;
    if (result == STORAGE_NO_SNAPSHOT) {
        console_write("no valid snapshot; live state unchanged\n");
        return;
    }
    if (result != STORAGE_OK) {
        console_write("storage load failed (error ");
        console_write_number((uint32_t)(-result));
        console_write("); live state unchanged\n");
        return;
    }

    storage_state_to_live(&storage_transfer_state);
    storage_generation = generation;
    storage_snapshot_loaded = 1;
    storage_snapshot_saved = 1;
    console_write("snapshot loaded generation ");
    console_write_number(storage_generation);
    console_putc('\n');
}

static void save_if_mutated(void) {
    if (mutation_succeeded) {
        storage_save_live_state();
    }
}

static void console_write_number(uint32_t value) {
    char digits[10];
    uint32_t length = 0;

    if (value == 0) {
        console_putc('0');
        return;
    }

    while (value != 0 && length < sizeof(digits)) {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while (length != 0) {
        console_putc(digits[--length]);
    }
}

static void list_patients(void) {
    if (patient_count == 0) {
        console_write("no patients registered\n");
        return;
    }

    for (uint32_t index = 0; index < patient_count; index++) {
        console_write("ID ");
        console_write_number(patients[index].id);
        console_write(", Age ");
        console_write_number(patients[index].age);
        console_write(", Name ");
        console_write(patients[index].name);
        console_putc('\n');
    }
}

static void show_patient(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;

    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage patient id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage patient id\n");
        return;
    }

    uint32_t index = find_patient(id);
    if (index == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }

    console_write("ID ");
    console_write_number(patients[index].id);
    console_write(", Age ");
    console_write_number(patients[index].age);
    console_write(", Name ");
    console_write(patients[index].name);
    console_putc('\n');
}

static void register_patient(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;
    uint32_t age;
    char name[PATIENT_NAME_CAPACITY];
    uint32_t name_length = 0;

    if (patient_count >= MAX_PATIENTS) {
        console_write("patient limit reached\n");
        return;
    }
    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage register id age name\n");
        return;
    }
    if (find_patient(id) != MAX_PATIENTS) {
        console_write("duplicate patient id\n");
        return;
    }
    if (!parse_number_token(&cursor, &age) || age < 1 || age > 120) {
        console_write("age must be 1 to 120\n");
        return;
    }

    skip_spaces(&cursor);
    while (cursor[name_length] != '\0') {
        name_length++;
    }
    while (name_length != 0 && cursor[name_length - 1] == ' ') {
        name_length--;
    }
    if (name_length == 0) {
        console_write("name required\n");
        return;
    }
    if (name_length >= PATIENT_NAME_CAPACITY) {
        console_write("name too long\n");
        return;
    }

    for (uint32_t index = 0; index < name_length; index++) {
        name[index] = cursor[index];
    }
    name[name_length] = '\0';

    patients[patient_count].id = id;
    patients[patient_count].age = age;
    patients[patient_count].in_use = 1;
    for (uint32_t index = 0; index <= name_length; index++) {
        patients[patient_count].name[index] = name[index];
    }
    patient_count++;
    mutation_succeeded = 1;
    console_write("patient registered\n");
}

static void remove_patient_assignment(uint32_t patient_id) {
    uint32_t index = find_assignment(patient_id);
    if (index == MAX_ASSIGNMENTS) {
        return;
    }

    assignments[index].patient_id = 0;
    assignments[index].doctor_id = 0;
    assignments[index].in_use = 0;
    assignment_count--;
}

static void remove_patient_records(uint32_t patient_id) {
    uint32_t write_index = 0;

    for (uint32_t read_index = 0; read_index < record_count; read_index++) {
        if (records[read_index].in_use &&
            records[read_index].patient_id == patient_id) {
            continue;
        }

        if (write_index != read_index) {
            records[write_index] = records[read_index];
        }
        write_index++;
    }

    uint32_t new_count = write_index;
    while (write_index < record_count) {
        records[write_index].id = 0;
        records[write_index].patient_id = 0;
        records[write_index].diagnosis[0] = '\0';
        records[write_index].in_use = 0;
        write_index++;
    }
    record_count = new_count;
}

static void delete_patient(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;

    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage delete id\n");
        return;
    }

    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage delete id\n");
        return;
    }

    uint32_t index = find_patient(id);
    if (index == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }

    remove_patient_records(id);
    remove_patient_assignment(id);
    for (uint32_t current = index; current + 1 < patient_count; current++) {
        patients[current] = patients[current + 1];
    }

    patient_count--;
    patients[patient_count].id = 0;
    patients[patient_count].age = 0;
    patients[patient_count].name[0] = '\0';
    patients[patient_count].in_use = 0;
    mutation_succeeded = 1;
    console_write("patient deleted\n");
}

static void write_record(const struct medical_record *record) {
    console_write("ID ");
    console_write_number(record->id);
    console_write(", Patient ");
    console_write_number(record->patient_id);
    console_write(", Diagnosis ");
    console_write(record->diagnosis);
    console_putc('\n');
}

static void add_record(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;
    uint32_t patient_id;
    uint32_t diagnosis_length = 0;

    if (record_count >= MAX_RECORDS) {
        console_write("record limit reached\n");
        return;
    }
    if (!parse_number_token(&cursor, &id) || id == 0 ||
        !parse_number_token(&cursor, &patient_id) || patient_id == 0) {
        console_write("usage record-add record_id patient_id diagnosis\n");
        return;
    }
    skip_spaces(&cursor);
    while (cursor[diagnosis_length] != '\0') {
        diagnosis_length++;
    }
    while (diagnosis_length != 0 && cursor[diagnosis_length - 1] == ' ') {
        diagnosis_length--;
    }
    if (diagnosis_length == 0) {
        console_write("diagnosis required\n");
        return;
    }
    if (diagnosis_length >= RECORD_DIAGNOSIS_CAPACITY) {
        console_write("diagnosis too long\n");
        return;
    }
    if (find_record(id) != MAX_RECORDS) {
        console_write("duplicate record id\n");
        return;
    }
    if (find_patient(patient_id) == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }

    records[record_count].id = id;
    records[record_count].patient_id = patient_id;
    records[record_count].in_use = 1;
    for (uint32_t index = 0; index < diagnosis_length; index++) {
        records[record_count].diagnosis[index] = cursor[index];
    }
    records[record_count].diagnosis[diagnosis_length] = '\0';
    record_count++;
    mutation_succeeded = 1;
    console_write("medical record added\n");
}

static void update_record(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;
    uint32_t diagnosis_length = 0;

    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage record-update record_id diagnosis\n");
        return;
    }
    skip_spaces(&cursor);
    while (cursor[diagnosis_length] != '\0') {
        diagnosis_length++;
    }
    while (diagnosis_length != 0 && cursor[diagnosis_length - 1] == ' ') {
        diagnosis_length--;
    }
    if (diagnosis_length == 0) {
        console_write("diagnosis required\n");
        return;
    }
    if (diagnosis_length >= RECORD_DIAGNOSIS_CAPACITY) {
        console_write("diagnosis too long\n");
        return;
    }

    uint32_t index = find_record(id);
    if (index == MAX_RECORDS) {
        console_write("record not found\n");
        return;
    }

    for (uint32_t character = 0; character < diagnosis_length; character++) {
        records[index].diagnosis[character] = cursor[character];
    }
    records[index].diagnosis[diagnosis_length] = '\0';
    mutation_succeeded = 1;
    console_write("medical record updated\n");
}

static void list_records(void) {
    if (record_count == 0) {
        console_write("no medical records\n");
        return;
    }

    for (uint32_t index = 0; index < record_count; index++) {
        write_record(&records[index]);
    }
}

static void list_patient_records(const char *arguments) {
    const char *cursor = arguments;
    uint32_t patient_id;
    uint32_t matching_records = 0;

    if (!parse_number_token(&cursor, &patient_id) || patient_id == 0) {
        console_write("usage records-patient patient_id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage records-patient patient_id\n");
        return;
    }
    if (find_patient(patient_id) == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }

    for (uint32_t index = 0; index < record_count; index++) {
        if (records[index].in_use && records[index].patient_id == patient_id) {
            write_record(&records[index]);
            matching_records++;
        }
    }
    if (matching_records == 0) {
        console_write("no medical records for patient\n");
    }
}

static void show_medical_history(const char *arguments) {
    const char *cursor = arguments;
    uint32_t patient_id;
    uint32_t patient_index;
    uint32_t matching_records = 0;

    if (!parse_number_token(&cursor, &patient_id) || patient_id == 0) {
        console_write("usage medical-history patient_id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage medical-history patient_id\n");
        return;
    }

    patient_index = find_patient(patient_id);
    if (patient_index == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }

    console_write("ID ");
    console_write_number(patients[patient_index].id);
    console_write(", Age ");
    console_write_number(patients[patient_index].age);
    console_write(", Name ");
    console_write(patients[patient_index].name);
    console_putc('\n');

    for (uint32_t index = 0; index < record_count; index++) {
        if (records[index].in_use && records[index].patient_id == patient_id) {
            console_write("Record ID ");
            console_write_number(records[index].id);
            console_write(", Diagnosis ");
            console_write(records[index].diagnosis);
            console_putc('\n');
            matching_records++;
        }
    }
    if (matching_records == 0) {
        console_write("no medical records for patient\n");
    }
}

static void show_ata_status(void) {
    uint32_t drive_count = ata_drive_count();

    console_write("ATA drives detected: ");
    console_write_number(drive_count);
    console_putc('\n');
    for (uint32_t index = 0; index < ATA_MAX_DRIVES; index++) {
        const struct ata_drive_info *drive = ata_drive_info(index);
        if (drive == (void *)0) {
            continue;
        }
        console_write("drive ");
        console_write_number(index);
        console_write(drive->channel == 0 ? " primary " : " secondary ");
        console_write(drive->device == 0 ? "master sectors " : "slave sectors ");
        console_write_number(drive->sector_count);
        console_putc('\n');
    }
}

static int ata_test_restore(void) {
    int result = ata_write_sector(ATA_TEST_DRIVE_INDEX, ATA_TEST_LBA,
                                  ata_test_original);
    if (result != 0) {
        console_write("ata-test: original sector restore failed (error ");
        console_write_number((uint32_t)(-result));
        console_write(")\n");
        return result;
    }
    return 0;
}

static void ata_sector_self_test(const char *arguments) {
    const struct ata_drive_info *drive;
    int result;
    uint8_t matches = 1;

    if (!strings_equal(arguments, "confirm")) {
        console_write("usage ata-test confirm\n");
        return;
    }

    drive = ata_drive_info(ATA_TEST_DRIVE_INDEX);
    if (drive == (void *)0 ||
        drive->sector_count != ATA_TEST_DISK_SECTORS) {
        console_write("ata-test refused: primary-master 64 MiB test disk required\n");
        return;
    }

    console_write("ata-test: testing reserved LBA ");
    console_write_number(ATA_TEST_LBA);
    console_write("\n");

    result = ata_read_sector(ATA_TEST_DRIVE_INDEX, ATA_TEST_LBA,
                             ata_test_original);
    if (result != 0) {
        console_write("ata-test: initial read failed (error ");
        console_write_number((uint32_t)(-result));
        console_write(")\n");
        return;
    }

    for (uint32_t index = 0; index < ATA_SECTOR_SIZE; index++) {
        ata_test_pattern[index] = (uint8_t)(0xa5u ^ (index * 37u));
    }

    result = ata_write_sector(ATA_TEST_DRIVE_INDEX, ATA_TEST_LBA,
                              ata_test_pattern);
    if (result != 0) {
        console_write("ata-test: pattern write failed (error ");
        console_write_number((uint32_t)(-result));
        console_write(")\n");
        (void)ata_test_restore();
        return;
    }

    result = ata_read_sector(ATA_TEST_DRIVE_INDEX, ATA_TEST_LBA,
                             ata_test_readback);
    if (result != 0) {
        console_write("ata-test: verification read failed (error ");
        console_write_number((uint32_t)(-result));
        console_write(")\n");
        (void)ata_test_restore();
        return;
    }

    for (uint32_t index = 0; index < ATA_SECTOR_SIZE; index++) {
        if (ata_test_readback[index] != ata_test_pattern[index]) {
            matches = 0;
            break;
        }
    }

    result = ata_test_restore();
    if (result != 0) {
        console_write("ata-test failed: original data may not be restored\n");
        return;
    }
    if (!matches) {
        console_write("ata-test failed: pattern mismatch; original sector restored\n");
        return;
    }
    console_write("ata-test passed; original sector restored\n");
}

static void show_record(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;

    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage record id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage record id\n");
        return;
    }

    uint32_t index = find_record(id);
    if (index == MAX_RECORDS) {
        console_write("record not found\n");
        return;
    }
    write_record(&records[index]);
}

static void delete_record(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;

    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage record-delete record_id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage record-delete record_id\n");
        return;
    }

    uint32_t index = find_record(id);
    if (index == MAX_RECORDS) {
        console_write("record not found\n");
        return;
    }

    for (uint32_t current = index; current + 1 < record_count; current++) {
        records[current] = records[current + 1];
    }
    record_count--;
    records[record_count].id = 0;
    records[record_count].patient_id = 0;
    records[record_count].diagnosis[0] = '\0';
    records[record_count].in_use = 0;
    mutation_succeeded = 1;
    console_write("medical record deleted\n");
}

static void list_doctors(void) {
    if (doctor_count == 0) {
        console_write("no doctors registered\n");
        return;
    }

    for (uint32_t index = 0; index < doctor_count; index++) {
        console_write("ID ");
        console_write_number(doctors[index].id);
        console_write(", Specialty ");
        console_write(doctors[index].specialty);
        console_write(", Name ");
        console_write(doctors[index].name);
        console_putc('\n');
    }
}

static void show_doctor(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;

    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage doctor id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage doctor id\n");
        return;
    }

    uint32_t index = find_doctor(id);
    if (index == MAX_DOCTORS) {
        console_write("doctor not found\n");
        return;
    }

    console_write("ID ");
    console_write_number(doctors[index].id);
    console_write(", Specialty ");
    console_write(doctors[index].specialty);
    console_write(", Name ");
    console_write(doctors[index].name);
    console_putc('\n');
}

static void add_doctor(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;
    char specialty[DOCTOR_SPECIALTY_CAPACITY];
    char name[DOCTOR_NAME_CAPACITY];
    uint32_t specialty_length = 0;
    uint32_t name_length = 0;

    if (doctor_count >= MAX_DOCTORS) {
        console_write("doctor limit reached\n");
        return;
    }
    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage doctor-add id specialty name\n");
        return;
    }
    if (find_doctor(id) != MAX_DOCTORS) {
        console_write("duplicate doctor id\n");
        return;
    }

    skip_spaces(&cursor);
    while (cursor[specialty_length] != '\0' &&
           cursor[specialty_length] != ' ') {
        specialty_length++;
    }
    if (specialty_length == 0) {
        console_write("specialty required\n");
        return;
    }
    if (specialty_length >= DOCTOR_SPECIALTY_CAPACITY) {
        console_write("specialty too long\n");
        return;
    }

    for (uint32_t index = 0; index < specialty_length; index++) {
        specialty[index] = cursor[index];
    }
    specialty[specialty_length] = '\0';

    cursor += specialty_length;
    skip_spaces(&cursor);
    while (cursor[name_length] != '\0') {
        name_length++;
    }
    while (name_length != 0 && cursor[name_length - 1] == ' ') {
        name_length--;
    }
    if (name_length == 0) {
        console_write("name required\n");
        return;
    }
    if (name_length >= DOCTOR_NAME_CAPACITY) {
        console_write("name too long\n");
        return;
    }

    for (uint32_t index = 0; index < name_length; index++) {
        name[index] = cursor[index];
    }
    name[name_length] = '\0';

    doctors[doctor_count].id = id;
    doctors[doctor_count].in_use = 1;
    for (uint32_t index = 0; index <= specialty_length; index++) {
        doctors[doctor_count].specialty[index] = specialty[index];
    }
    for (uint32_t index = 0; index <= name_length; index++) {
        doctors[doctor_count].name[index] = name[index];
    }
    doctor_count++;
    mutation_succeeded = 1;
    console_write("doctor registered\n");
}

static void assign_doctor(const char *arguments) {
    const char *cursor = arguments;
    uint32_t patient_id;
    uint32_t doctor_id;

    if (!parse_number_token(&cursor, &patient_id) || patient_id == 0 ||
        !parse_number_token(&cursor, &doctor_id) || doctor_id == 0) {
        console_write("usage assign patient_id doctor_id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage assign patient_id doctor_id\n");
        return;
    }
    if (find_patient(patient_id) == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }
    if (find_doctor(doctor_id) == MAX_DOCTORS) {
        console_write("doctor not found\n");
        return;
    }

    uint32_t existing = find_assignment(patient_id);
    if (existing != MAX_ASSIGNMENTS) {
        if (assignments[existing].doctor_id == doctor_id) {
            console_write("assignment already exists\n");
        } else {
            console_write("patient already has a doctor\n");
        }
        return;
    }
    if (assignment_count >= MAX_ASSIGNMENTS) {
        console_write("assignment limit reached\n");
        return;
    }

    for (uint32_t index = 0; index < MAX_ASSIGNMENTS; index++) {
        if (!assignments[index].in_use) {
            assignments[index].patient_id = patient_id;
            assignments[index].doctor_id = doctor_id;
            assignments[index].in_use = 1;
            assignment_count++;
            mutation_succeeded = 1;
            console_write("doctor assigned\n");
            return;
        }
    }

    console_write("assignment limit reached\n");
}

static void list_assignments(void) {
    if (assignment_count == 0) {
        console_write("no doctor assignments\n");
        return;
    }

    for (uint32_t index = 0; index < MAX_ASSIGNMENTS; index++) {
        if (!assignments[index].in_use) {
            continue;
        }
        console_write("Patient ");
        console_write_number(assignments[index].patient_id);
        console_write(" -> Doctor ");
        console_write_number(assignments[index].doctor_id);
        console_putc('\n');
    }
}

static void show_patient_doctor(const char *arguments) {
    const char *cursor = arguments;
    uint32_t patient_id;

    if (!parse_number_token(&cursor, &patient_id) || patient_id == 0) {
        console_write("usage patient-doctor patient_id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage patient-doctor patient_id\n");
        return;
    }
    if (find_patient(patient_id) == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }

    uint32_t assignment = find_assignment(patient_id);
    if (assignment == MAX_ASSIGNMENTS) {
        console_write("patient has no assigned doctor\n");
        return;
    }

    console_write("Patient ");
    console_write_number(patient_id);
    console_write(" -> Doctor ");
    console_write_number(assignments[assignment].doctor_id);
    console_putc('\n');
}

static void unassign_doctor(const char *arguments) {
    const char *cursor = arguments;
    uint32_t patient_id;

    if (!parse_number_token(&cursor, &patient_id) || patient_id == 0) {
        console_write("usage unassign patient_id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage unassign patient_id\n");
        return;
    }
    if (find_patient(patient_id) == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }

    uint32_t assignment = find_assignment(patient_id);
    if (assignment == MAX_ASSIGNMENTS) {
        console_write("patient has no assigned doctor\n");
        return;
    }

    assignments[assignment].patient_id = 0;
    assignments[assignment].doctor_id = 0;
    assignments[assignment].in_use = 0;
    assignment_count--;
    mutation_succeeded = 1;
    console_write("doctor unassigned\n");
}

static void write_appointment(const struct appointment *appointment) {
    console_write("ID ");
    console_write_number(appointment->id);
    console_write(", Patient ");
    console_write_number(appointment->patient_id);
    console_write(", Doctor ");
    console_write_number(appointment->doctor_id);
    console_write(", Slot ");
    console_write_number(appointment->slot);
    console_putc('\n');
}

static void list_appointments(void) {
    if (appointment_count == 0) {
        console_write("no appointments scheduled\n");
        return;
    }

    for (uint32_t index = 0; index < appointment_count; index++) {
        write_appointment(&appointments[index]);
    }
}

static void show_appointment(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;

    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage appointment id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage appointment id\n");
        return;
    }

    uint32_t index = find_appointment(id);
    if (index == MAX_APPOINTMENTS) {
        console_write("appointment not found\n");
        return;
    }

    write_appointment(&appointments[index]);
}

static void add_appointment(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;
    uint32_t patient_id;
    uint32_t doctor_id;
    uint32_t slot;

    if (appointment_count >= MAX_APPOINTMENTS) {
        console_write("appointment limit reached\n");
        return;
    }
    if (!parse_number_token(&cursor, &id) || id == 0 ||
        !parse_number_token(&cursor, &patient_id) || patient_id == 0 ||
        !parse_number_token(&cursor, &doctor_id) || doctor_id == 0 ||
        !parse_number_token(&cursor, &slot) || slot == 0) {
        console_write("usage appointment-add appointment_id patient_id doctor_id slot\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage appointment-add appointment_id patient_id doctor_id slot\n");
        return;
    }
    if (find_appointment(id) != MAX_APPOINTMENTS) {
        console_write("duplicate appointment id\n");
        return;
    }
    if (find_patient(patient_id) == MAX_PATIENTS) {
        console_write("patient not found\n");
        return;
    }
    if (find_doctor(doctor_id) == MAX_DOCTORS) {
        console_write("doctor not found\n");
        return;
    }

    for (uint32_t index = 0; index < appointment_count; index++) {
        if (appointments[index].doctor_id == doctor_id &&
            appointments[index].slot == slot) {
            console_write("doctor already booked for slot\n");
            return;
        }
        if (appointments[index].patient_id == patient_id &&
            appointments[index].slot == slot) {
            console_write("patient already booked for slot\n");
            return;
        }
    }

    appointments[appointment_count].id = id;
    appointments[appointment_count].patient_id = patient_id;
    appointments[appointment_count].doctor_id = doctor_id;
    appointments[appointment_count].slot = slot;
    appointments[appointment_count].in_use = 1;
    appointment_count++;
    mutation_succeeded = 1;
    console_write("appointment scheduled\n");
}

static void cancel_appointment(const char *arguments) {
    const char *cursor = arguments;
    uint32_t id;

    if (!parse_number_token(&cursor, &id) || id == 0) {
        console_write("usage appointment-cancel id\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage appointment-cancel id\n");
        return;
    }

    uint32_t index = find_appointment(id);
    if (index == MAX_APPOINTMENTS) {
        console_write("appointment not found\n");
        return;
    }

    for (uint32_t current = index; current + 1 < appointment_count; current++) {
        appointments[current] = appointments[current + 1];
    }
    appointment_count--;
    appointments[appointment_count].id = 0;
    appointments[appointment_count].patient_id = 0;
    appointments[appointment_count].doctor_id = 0;
    appointments[appointment_count].slot = 0;
    appointments[appointment_count].in_use = 0;
    mutation_succeeded = 1;
    console_write("appointment cancelled\n");
}

static void banker_write_vector(const uint32_t vector[BANKER_RESOURCES]) {
    console_write("ICU ");
    console_write_number(vector[0]);
    console_write(" Vent ");
    console_write_number(vector[1]);
    console_write(" Monitor ");
    console_write_number(vector[2]);
    console_putc('\n');
}

static uint8_t banker_is_safe(uint32_t sequence[BANKER_PROCESSES],
                              uint32_t *sequence_length) {
    uint32_t work[BANKER_RESOURCES];
    uint8_t finished[BANKER_PROCESSES] = {0, 0, 0, 0, 0};
    uint32_t length = 0;

    for (uint32_t resource = 0; resource < BANKER_RESOURCES; resource++) {
        work[resource] = banker_available[resource];
    }

    for (;;) {
        uint8_t progress = 0;

        for (uint32_t process = 0; process < BANKER_PROCESSES; process++) {
            if (finished[process] != 0 ||
                banker_need[process][0] > work[0] ||
                banker_need[process][1] > work[1] ||
                banker_need[process][2] > work[2]) {
                continue;
            }

            work[0] += banker_allocation[process][0];
            work[1] += banker_allocation[process][1];
            work[2] += banker_allocation[process][2];
            finished[process] = 1;
            sequence[length++] = process;
            progress = 1;
        }

        if (progress == 0 || length == BANKER_PROCESSES) {
            break;
        }
    }

    *sequence_length = length;
    return length == BANKER_PROCESSES;
}

static void banker_write_safety(void) {
    uint32_t sequence[BANKER_PROCESSES];
    uint32_t sequence_length;

    if (!banker_is_safe(sequence, &sequence_length)) {
        console_write("unsafe state\n");
        return;
    }

    console_write("safe sequence ");
    for (uint32_t index = 0; index < sequence_length; index++) {
        console_write("P");
        console_write_number(sequence[index]);
        if (index + 1 < sequence_length) {
            console_write(" ");
        }
    }
    console_putc('\n');
}

static void banker_status(void) {
    console_write("total resources\n");
    banker_write_vector(banker_total);
    console_write("available resources\n");
    banker_write_vector(banker_available);
    banker_write_safety();
}

static void banker_write_matrix(const char *title,
                                uint32_t matrix[BANKER_PROCESSES][BANKER_RESOURCES]) {
    console_write(title);
    console_putc('\n');
    for (uint32_t process = 0; process < BANKER_PROCESSES; process++) {
        console_write("P");
        console_write_number(process);
        console_write(" ");
        banker_write_vector(matrix[process]);
    }
}

static void banker_matrix(void) {
    banker_write_matrix("allocation", banker_allocation);
    banker_write_matrix("maximum", banker_maximum);
    banker_write_matrix("need", banker_need);
    banker_status();
}

static void banker_request(const char *arguments) {
    const char *cursor = arguments;
    uint32_t process;
    uint32_t request[BANKER_RESOURCES];
    uint32_t sequence[BANKER_PROCESSES];
    uint32_t sequence_length;

    if (!parse_number_token(&cursor, &process) || process >= BANKER_PROCESSES ||
        !parse_number_token(&cursor, &request[0]) ||
        !parse_number_token(&cursor, &request[1]) ||
        !parse_number_token(&cursor, &request[2])) {
        console_write("usage banker-request process icu ventilator monitor\n");
        return;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        console_write("usage banker-request process icu ventilator monitor\n");
        return;
    }
    if (request[0] == 0 && request[1] == 0 && request[2] == 0) {
        console_write("request must be nonzero\n");
        return;
    }
    if (request[0] > banker_need[process][0] ||
        request[1] > banker_need[process][1] ||
        request[2] > banker_need[process][2]) {
        console_write("request exceeds remaining need\n");
        return;
    }
    if (request[0] > banker_available[0] ||
        request[1] > banker_available[1] ||
        request[2] > banker_available[2]) {
        console_write("resources currently unavailable\n");
        return;
    }

    for (uint32_t resource = 0; resource < BANKER_RESOURCES; resource++) {
        banker_available[resource] -= request[resource];
        banker_allocation[process][resource] += request[resource];
        banker_need[process][resource] -= request[resource];
    }

    if (!banker_is_safe(sequence, &sequence_length)) {
        for (uint32_t resource = 0; resource < BANKER_RESOURCES; resource++) {
            banker_available[resource] += request[resource];
            banker_allocation[process][resource] -= request[resource];
            banker_need[process][resource] += request[resource];
        }
        console_write("request denied unsafe state\n");
        return;
    }

    console_write("request granted\n");
    mutation_succeeded = 1;
    banker_write_safety();
}

static void execute_command(void) {
    const char *arguments;

    console_newline();
    mutation_succeeded = 0;

    if (strings_equal(command_buffer, "help")) {
        console_write("help clear version\n");
        console_write("register id age name\n");
        console_write("patients\n");
        console_write("patient id\n");
        console_write("delete id\n");
        console_write("doctor-add id specialty name\n");
        console_write("doctors\n");
        console_write("doctor id\n");
        console_write("assign patient_id doctor_id\n");
        console_write("assignments\n");
        console_write("patient-doctor patient_id\n");
        console_write("unassign patient_id\n");
        console_write("record-add record_id patient_id diagnosis\n");
        console_write("record-update record_id new_diagnosis\n");
        console_write("records\n");
        console_write("records-patient patient_id\n");
        console_write("medical-history patient_id\n");
        console_write("record record_id\n");
        console_write("record-delete record_id\n");
        console_write("appointment-add appointment_id patient_id doctor_id slot\n");
        console_write("appointments\n");
        console_write("appointment appointment_id\n");
        console_write("appointment-cancel appointment_id\n");
        console_write("banker-status\n");
        console_write("banker-matrix\n");
        console_write("banker-request process icu ventilator monitor\n");
        console_write("storage-status\n");
        console_write("storage-save\n");
        console_write("storage-load\n");
        console_write("ata-status\n");
        console_write("ata-test confirm\n");
    } else if (strings_equal(command_buffer, "clear")) {
        console_clear();
    } else if (strings_equal(command_buffer, "version")) {
        console_write("HospitalOS v0.1\n");
    } else if (strings_equal(command_buffer, "patients")) {
        list_patients();
    } else if (strings_equal(command_buffer, "doctors")) {
        list_doctors();
    } else if (strings_equal(command_buffer, "assignments")) {
        list_assignments();
    } else if (strings_equal(command_buffer, "records")) {
        list_records();
    } else if (strings_equal(command_buffer, "appointments")) {
        list_appointments();
    } else if (strings_equal(command_buffer, "banker-status")) {
        banker_status();
    } else if (strings_equal(command_buffer, "banker-matrix")) {
        banker_matrix();
    } else if (strings_equal(command_buffer, "storage-status")) {
        storage_status();
    } else if (strings_equal(command_buffer, "storage-save")) {
        storage_save_command();
    } else if (strings_equal(command_buffer, "storage-load")) {
        storage_load_command();
    } else if (strings_equal(command_buffer, "ata-status")) {
        show_ata_status();
    } else if ((arguments = command_arguments("ata-test")) != (void *)0) {
        ata_sector_self_test(arguments);
    } else if ((arguments = command_arguments("patient")) != (void *)0) {
        show_patient(arguments);
    } else if ((arguments = command_arguments("doctor-add")) != (void *)0) {
        add_doctor(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("doctor")) != (void *)0) {
        show_doctor(arguments);
    } else if ((arguments = command_arguments("assign")) != (void *)0) {
        assign_doctor(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("patient-doctor")) != (void *)0) {
        show_patient_doctor(arguments);
    } else if ((arguments = command_arguments("unassign")) != (void *)0) {
        unassign_doctor(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("record-add")) != (void *)0) {
        add_record(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("record-delete")) != (void *)0) {
        delete_record(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("record-update")) != (void *)0) {
        update_record(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("records-patient")) != (void *)0) {
        list_patient_records(arguments);
    } else if ((arguments = command_arguments("medical-history")) != (void *)0) {
        show_medical_history(arguments);
    } else if ((arguments = command_arguments("record")) != (void *)0) {
        show_record(arguments);
    } else if ((arguments = command_arguments("appointment-add")) != (void *)0) {
        add_appointment(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("appointment-cancel")) != (void *)0) {
        cancel_appointment(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("appointment")) != (void *)0) {
        show_appointment(arguments);
    } else if ((arguments = command_arguments("banker-request")) != (void *)0) {
        banker_request(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("register")) != (void *)0) {
        register_patient(arguments);
        save_if_mutated();
    } else if ((arguments = command_arguments("delete")) != (void *)0) {
        delete_patient(arguments);
        save_if_mutated();
    } else if (command_length != 0) {
        console_write("unknown command\n");
    }

    command_length = 0;
    for (uint64_t index = 0; index < COMMAND_CAPACITY; index++) {
        command_buffer[index] = '\0';
    }
    console_prompt();
}

static inline uint8_t port_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t keyboard_read_scancode(void) {
    if ((port_in8(0x64) & 1u) == 0) {
        return 0;
    }
    return port_in8(0x60);
}

static char scancode_to_character(uint8_t scancode) {
    char character = '\0';

    switch (scancode) {
        case 0x02: character = '1'; break;
        case 0x03: character = '2'; break;
        case 0x04: character = '3'; break;
        case 0x05: character = '4'; break;
        case 0x06: character = '5'; break;
        case 0x07: character = '6'; break;
        case 0x08: character = '7'; break;
        case 0x09: character = '8'; break;
        case 0x0a: character = '9'; break;
        case 0x0b: character = '0'; break;
        case 0x0c: character = '-'; break;
        case 0x10: character = 'q'; break;
        case 0x11: character = 'w'; break;
        case 0x12: character = 'e'; break;
        case 0x13: character = 'r'; break;
        case 0x14: character = 't'; break;
        case 0x15: character = 'y'; break;
        case 0x16: character = 'u'; break;
        case 0x17: character = 'i'; break;
        case 0x18: character = 'o'; break;
        case 0x19: character = 'p'; break;
        case 0x1e: character = 'a'; break;
        case 0x1f: character = 's'; break;
        case 0x20: character = 'd'; break;
        case 0x21: character = 'f'; break;
        case 0x22: character = 'g'; break;
        case 0x23: character = 'h'; break;
        case 0x24: character = 'j'; break;
        case 0x25: character = 'k'; break;
        case 0x26: character = 'l'; break;
        case 0x2c: character = 'z'; break;
        case 0x2d: character = 'x'; break;
        case 0x2e: character = 'c'; break;
        case 0x2f: character = 'v'; break;
        case 0x30: character = 'b'; break;
        case 0x31: character = 'n'; break;
        case 0x32: character = 'm'; break;
        case 0x39: character = ' '; break;
        default: return '\0';
    }

    if (keyboard_shift && character >= 'a' && character <= 'z') {
        character = (char)(character - ('a' - 'A'));
    }
    return character;
}

static void keyboard_process(void) {
    uint8_t scancode = keyboard_read_scancode();
    if (scancode == 0) {
        return;
    }

    /* Set 1 extended navigation keys use E0 followed by a raw make/break byte. */
    if (keyboard_extended) {
        keyboard_extended = 0;
        if (scancode == 0x49) {
            console_page_up();
        } else if (scancode == 0x51) {
            console_page_down();
        } else if (scancode == 0xc9 || scancode == 0xd1) {
            /* Page Up/Page Down break codes: consume without input. */
        }
        return;
    }

    if (scancode == 0xe0) {
        keyboard_extended = 1;
        return;
    }

    uint8_t released = scancode & 0x80u;
    uint8_t code = scancode & 0x7fu;

    if (code == 0x2a || code == 0x36) {
        keyboard_shift = released ? 0 : 1;
        keyboard_extended = 0;
        return;
    }

    if (released) {
        return;
    }

    /* Some virtual keyboard paths expose the navigation make byte without E0. */
    if (code == 0x49) {
        console_page_up();
        return;
    }
    if (code == 0x51) {
        console_page_down();
        return;
    }

    if (code == 0x1c) {
        execute_command();
        return;
    }
    if (code == 0x0e) {
        if (command_length != 0) {
            command_length--;
            command_buffer[command_length] = '\0';
            console_backspace();
        }
        return;
    }

    char character = scancode_to_character(code);
    if (character != '\0' && command_length < COMMAND_CAPACITY - 1) {
        command_buffer[command_length++] = character;
        command_buffer[command_length] = '\0';
        console_putc(character);
    }
}

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* Entry point required by kernel/linker.ld. */
void kmain(void) {
    active_framebuffer = framebuffer();

    if (active_framebuffer == (void *)0 ||
        active_framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        active_framebuffer->bpp != 32) {
        halt_forever();
    }

    if (active_framebuffer->width <= CONSOLE_MARGIN * 2 + GLYPH_ADVANCE_X ||
        active_framebuffer->height <= CONSOLE_MARGIN * 2 + GLYPH_ADVANCE_Y) {
        halt_forever();
    }
    console_columns = (active_framebuffer->width - CONSOLE_MARGIN * 2) /
                      GLYPH_ADVANCE_X;
    console_rows = (active_framebuffer->height - CONSOLE_MARGIN * 2) /
                   GLYPH_ADVANCE_Y;
    if (console_columns > CONSOLE_HISTORY_COLUMNS) {
        console_columns = CONSOLE_HISTORY_COLUMNS;
    }

    console_clear();
    storage_last_result = storage_initialize();
    if (storage_last_result == STORAGE_OK) {
        storage_available = 1;
        storage_last_result = storage_load(&storage_transfer_state,
                                           &storage_generation);
        if (storage_last_result == STORAGE_OK) {
            storage_state_to_live(&storage_transfer_state);
            storage_snapshot_loaded = 1;
            storage_snapshot_saved = 1;
            console_write("storage: snapshot loaded generation ");
            console_write_number(storage_generation);
            console_putc('\n');
        } else if (storage_last_result == STORAGE_NO_SNAPSHOT) {
            console_write("storage: no valid snapshot; using in-memory defaults\n");
        } else {
            console_write("storage: snapshot load failed; using in-memory defaults\n");
        }
    } else {
        storage_available = 0;
        console_write("storage: unavailable; using in-memory data\n");
    }
    console_write("HospitalOS booted\n");
    console_prompt();

    for (;;) {
        keyboard_process();
        __asm__ volatile ("pause");
    }
}
