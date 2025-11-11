/**
 * Flappy Bird - Mini LCD Game
 * Column Movement Test (Bird Added) - Waits for input before starting.
 */

#undef F_CPU
#define F_CPU 16000000
#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega128");

#define __AVR_ATmega128__ 1
#include <avr/io.h>
#include <util/delay.h> 

// GENERAL INIT - USED BY ALMOST EVERYTHING ----------------------------------

static void port_init() {
    PORTA = 0b00011111; DDRA = 0b01000000; // buttons & led
    PORTB = 0b00000000; DDRB = 0b00000000;
    PORTC = 0b00000000; DDRC = 0b11110111; // lcd
    PORTD = 0b11000000; DDRD = 0b00001000;
    PORTE = 0b00100000; DDRE = 0b00110000; // buzzer
    PORTF = 0b00000000; DDRF = 0b00000000;
    PORTG = 0b00000000; DDRG = 0b00000000;
}

// TIMER-BASED RANDOM NUMBER GENERATOR ---------------------------------------

static void rnd_init() {
    TCCR0 |= (1 << CS00);   
    TCNT0 = 0;              
}

static int rnd_gen(int max) {
    return (TCNT0 + TCNT1) % max;
}

// BUTTON HANDLING -----------------------------------------------------------

#define BUTTON_NONE     0
#define BUTTON_CENTER   1
#define BUTTON_UP       4 // Unused, but kept for consistency
static int button_accept = 1;

static int button_pressed() {
    // UP button (PIN A1)
    if (!(PINA & 0b00000010) && button_accept) { 
        button_accept = 0; 
        return BUTTON_UP;
    }
    // CENTER button (PIN A2)
    if (!(PINA & 0b00000100) && button_accept) { 
        button_accept = 0; 
        return BUTTON_CENTER;
    }
    return BUTTON_NONE;
}

static void button_unlock() {
    // Check if both UP and CENTER buttons are released before re-enabling input
    if ((PINA & 0b00000010) && (PINA & 0b00000100))
    button_accept = 1;
}

// LCD HELPERS ---------------------------------------------------------------

#define CLR_DISP        0x00000001
#define DISP_ON         0x0000000C
#define DD_RAM_ADDR     0x00000080
#define DD_RAM_ADDR2    0x000000C0

static void lcd_delay(unsigned int b) {
    volatile unsigned int a = b;
    while (a) a--;
}

static void lcd_pulse() {
    PORTC = PORTC | 0b00000100; // E high
    lcd_delay(1400);            
    PORTC = PORTC & 0b11111011; // E low
}

// Fixed robust 4-bit send function
static void lcd_send(int command, unsigned char a) {
    unsigned char data;

    // Send high 4 bits
    data = (a & 0xF0) | (PORTC & 0x0F); 
    PORTC = (PORTC & 0x0F) | data;
    if (command)
        PORTC = PORTC & 0b11111110; 
    else
        PORTC = PORTC | 0b00000001; 
    lcd_pulse(); 

    // Send low 4 bits
    data = (a << 4) | (PORTC & 0x0F); 
    PORTC = (PORTC & 0x0F) | data;
    if (command)
        PORTC = PORTC & 0b11111110; 
    else
        PORTC = PORTC | 0b00000001; 
    lcd_pulse(); 
}

static void lcd_send_command(unsigned char a) {
    lcd_send(1, a);
}

static void lcd_send_data(unsigned char a) {
    lcd_send(0, a);
}

static void lcd_init() {
    PORTC = PORTC & 0b11111110;
    lcd_delay(10000);
    PORTC = 0b00110000; lcd_pulse(); lcd_delay(1000);
    PORTC = 0b00110000; lcd_pulse(); lcd_delay(1000);
    PORTC = 0b00110000; lcd_pulse(); lcd_delay(1000);
    PORTC = 0b00100000; lcd_pulse();
    lcd_send_command(0x28); // 4 bits, 2 lines, 5x8 font
    lcd_send_command(0x08); // display off
    lcd_send_command(CLR_DISP); // clear display
    lcd_send_command(0x06); // entry mode set
    lcd_send_command(DISP_ON);
    lcd_send_command(CLR_DISP);
}

static void lcd_send_line1(char *str) {
    lcd_send_command(DD_RAM_ADDR);
    while (*str) lcd_send_data(*str++);
}

static void lcd_send_line2(char *str) {
    lcd_send_command(DD_RAM_ADDR2);
    while (*str) lcd_send_data(*str++);
}

// PIPE MOVEMENT LOGIC -------------------------------------------------------

#define COLS 16
#define VIRTUAL_ROWS 8
#define PIPE_GAP_SIZE 3
static unsigned char pipe_gaps[COLS]; // 9 means no pipe

// Bird State
#define BIRD_COL 3          // Bird is always drawn at column 3
#define BIRD_FALLING 0
#define BIRD_STABILIZED 1
#define BIRD_GOING_UP 2

static int bird_vrow;       // Bird's top virtual row (0 to VIRTUAL_ROWS - 2)
static int bird_state;

// Timing
#define PIPE_SHIFT_DELAY 3  // How many loops between column shifts
#define PIPE_SPAWN_COL (COLS - 1) 
#define BIRD_GRAVITY_DELAY 5 // Bird falls every 5 game loops (0.5s at 100ms loop)

static void game_init() {
    for (int i = 0; i < COLS; ++i) {
        pipe_gaps[i] = 9; 
    }
    // Set initial bird position (forth row, index 3) and state
    bird_vrow = 3; 
    bird_state = BIRD_FALLING;
}

// Shifts the entire pipe array one column to the left and spawns new pipes.
static void pipe_shift() {
    // 1. Shift all pipes one column left
    for (int i = 0; i < COLS - 1; ++i) {
        pipe_gaps[i] = pipe_gaps[i + 1];
    }
    
    // 2. Clear the last column
    pipe_gaps[PIPE_SPAWN_COL] = 9;
    
    // 3. Spawn a new pipe occasionally 
    // Wait for at least 5 columns to clear before spawning a new one.
    if (pipe_gaps[COLS - 6] == 9) {
        
        // New Pipe Generation Logic: Ensure the vertical jump is at most 1 virtual row (max step of +/- 1).
        int prev_pipe_pos = pipe_gaps[COLS - 5]; 
        // Default to a central base position (3) if no previous pipe
        int base_pos = (prev_pipe_pos != 9) ? prev_pipe_pos : 3;

        // Determine the safe range [min_pos, max_pos]
        int min_pos = base_pos - 1;
        if (min_pos < 1) min_pos = 1;

        int max_pos = base_pos + 1;
        if (max_pos > 4) max_pos = 4;
        
        // The number of choices available in the range [min_pos, max_pos]
        int num_choices = max_pos - min_pos + 1;

        // Generate a random number from 0 to num_choices - 1, and shift the result by min_pos
        int new_gap_start = rnd_gen(num_choices) + min_pos;

        pipe_gaps[PIPE_SPAWN_COL] = new_gap_start; 
    }
}

// CUSTOM CHARACTERS AND RENDERING -------------------------------------------

#define CHAR_SOLID     0
#define CHAR_TOP_HALF    1
#define CHAR_BOTTOM_HALF  2
#define CHAR_BIRD_BOTTOM_PIPE_TOP 3 // bird bottom + full top
#define CHAR_BIRD_TOP_PIPE_BOTTOM 4 // bird top + full bottom
#define CHAR_BIRD_TOP_EMPTY_BOTTOM 5 // bird top + empty bottom
#define CHAR_BIRD_BOTTOM_EMPTY_TOP 6 // bird bottom + empty top

#define CHARMAP_SIZE    7 

static unsigned char CHARMAP[CHARMAP_SIZE][8] = {
    { // 0: CHAR_SOLID
        0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111
    },
    { // 1: CHAR_TOP_HALF 
        0b11111, 0b11111, 0b11111, 0b11111, 0b00000, 0b00000, 0b00000, 0b00000
    },
    { // 2: CHAR_BOTTOM_HALF 
        0b00000, 0b00000, 0b00000, 0b00000, 0b11111, 0b11111, 0b11111, 0b11111
    },
    { // 3: CHAR_BIRD_BOTTOM_PIPE_TOP (bird bottom + full top)
        0b11111, 0b11111, 0b11111, 0b11111, 0b00000, 0b01100, 0b01100, 0b00000
    },
    { // 4: CHAR_BIRD_TOP_PIPE_BOTTOM (bird top + full bottom)
        0b00000, 0b01100, 0b01100, 0b00000, 0b11111, 0b11111, 0b11111, 0b11111
    },
    { // 5: CHAR_BIRD_TOP_EMPTY_BOTTOM (bird top + empty bottom)
        0b00000, 0b01100, 0b01100, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000
    },
    { // 6: CHAR_BIRD_BOTTOM_EMPTY_TOP (bird bottom + empty top)
        0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100, 0b00000
    }
};

static void chars_init() {
    for (int c = 0; c < CHARMAP_SIZE; ++c) {
        lcd_send_command(0x40 + c*8);
        for (int r = 0; r < 8; ++r)
            lcd_send_data(CHARMAP[c][r]);
    }
}

// Determines which custom character or space to display for a column/line position.
static unsigned char get_char_for_col(int col, int line) {
    // vrow_start is the first virtual row covered by this character (0 or 4)
    int vrow_start = line * 4; 
    int gap_start = pipe_gaps[col];
    
    // --- BIRD DRAWING LOGIC (Priority in BIRD_COL) ---
    if (col == BIRD_COL) {
        
        // CRITICAL FIX: The bird is 2 vrows tall, and the custom characters only allow 
        // drawing it aligned to the top (0/1) or bottom (2/3) half of the 4-vrow cell.
        // We quantize the bird's vrow position for drawing only to the nearest even number
        // (0, 2, 4, 6) to ensure perfect alignment and prevent duplication/split issues.
        int bird_vrow_draw = (bird_vrow / 2) * 2; 

        // Check if the bird's 2-vrow range overlaps with this 5x8 character's 4-vrow range
        // Bird occupies virtual rows: [bird_vrow_draw] and [bird_vrow_draw + 1]
        // Character occupies virtual rows: [vrow_start] through [vrow_start + 3]
        if (bird_vrow_draw + 1 >= vrow_start && bird_vrow_draw < vrow_start + 4) {

            // Bird is in this character cell. Now determine pipe background.
            
            // Check pipe status for the two halves of the character
            int gap_end = gap_start + PIPE_GAP_SIZE;
            
            // Half 1 (Top 4 rows of char): Covers vrows vrow_start and vrow_start + 1 
            int is_half1_gap = (vrow_start < gap_end && vrow_start + 1 >= gap_start);
            
            // Half 2 (Bottom 4 rows of char): Covers vrows vrow_start + 2 and vrow_start + 3 
            int is_half2_gap = (vrow_start + 2 < gap_end && vrow_start + 3 >= gap_start);

            // Determine if pipe parts are solid, but only if a pipe exists (gap_start != 9)
            int half1_solid = (gap_start != 9) && !is_half1_gap;
            int half2_solid = (gap_start != 9) && !is_half2_gap;
            
            // Determine bird position within the character cell
            // The bird's top virtual row, relative to the start of this 5x8 character's 4 virtual rows.
            int vrow_offset = bird_vrow_draw - vrow_start; 
            
            // If vrow_offset < 2 (i.e., 0 or 1), the bird starts in the top half of the 4-vrow cell.
            int bird_is_top_half_of_char_cell = (vrow_offset < 2);
            
            if (bird_is_top_half_of_char_cell) {
                // Bird is drawn in the character's top half (vrow 0/1 or 4/5)
                if (half2_solid) {
                    return CHAR_BIRD_TOP_PIPE_BOTTOM; // Custom character 4: Bird Top + Pipe Bottom
                } else {
                    return CHAR_BIRD_TOP_EMPTY_BOTTOM; // Custom character 5: Bird Top + Empty Bottom
                }
            } else {
                // Bird is drawn in the character's bottom half (vrow 2/3 or 6/7)
                if (half1_solid) {
                    return CHAR_BIRD_BOTTOM_PIPE_TOP; // Custom character 3: Bird Bottom + Pipe Top
                } else {
                    return CHAR_BIRD_BOTTOM_EMPTY_TOP; // Custom character 6: Bird Bottom + Empty Top
                }
            }
        }
    }
    
    // --- PIPE/EMPTY DRAWING (for all columns, and as fallback for BIRD_COL when bird is not in this cell) ---
    
    if (gap_start == 9) {
        return ' '; // Column is empty (fixes the bug where bird was missed in empty space)
    }

    // Pipe logic:
    int gap_end = gap_start + PIPE_GAP_SIZE;
    // Check if the two halves of the 5x8 character are in the gap
    int is_half1_gap = (vrow_start < gap_end && vrow_start + 1 >= gap_start);
    int is_half2_gap = (vrow_start + 2 < gap_end && vrow_start + 3 >= gap_start);

    int half1_solid = !is_half1_gap;
    int half2_solid = !is_half2_gap;

    if (half1_solid && half2_solid) return CHAR_SOLID;
    if (half1_solid) return CHAR_TOP_HALF;
    if (half2_solid) return CHAR_BOTTOM_HALF;
    
    return ' '; // Entire character is within the gap
}


static void screen_update() {
    // Line 1 (vrows 0-3)
    lcd_send_command(DD_RAM_ADDR);
    for (int col = 0; col < COLS; ++col) {
        lcd_send_data(get_char_for_col(col, 0));
    }

    // Line 2 (vrows 4-7)
    lcd_send_command(DD_RAM_ADDR2);
    for (int col = 0; col < COLS; ++col) {
        lcd_send_data(get_char_for_col(col, 1));
    }
}


// THE MAIN LOOP =============================================================

int main() {
    port_init();
    lcd_init();
    chars_init();
    rnd_init();

    // Initial splash screen and prompt
    lcd_send_line1("  Column Test");
    lcd_send_line2("Press Center"); 

    game_init(); 
    
    // **WAIT FOR START INPUT**
    while (1) {
        if (button_pressed() == BUTTON_CENTER) {
            button_unlock(); 
            break;
        }
        button_unlock();
        _delay_ms(10); // Short delay to prevent busy-waiting
    }
    
    lcd_send_command(CLR_DISP); // Clear display before starting the scroll
    
    int pipe_shift_counter = 0;
    int gravity_counter = 0;

    // Loop for pipe movement demonstration
    while (1) {
        // --- 0. Handle Input & Bird State Change ---
        int input = button_pressed();
        if (input == BUTTON_CENTER) {
            // Flap: Always move up one virtual row (if possible) and enter the GOING_UP state
            if (bird_vrow > 0) {
                bird_vrow--;
            }
            bird_state = BIRD_GOING_UP;
        }
        button_unlock();

        // --- 1. Pipe Movement (Column Shift) ---
        if (++pipe_shift_counter >= PIPE_SHIFT_DELAY) {
            pipe_shift_counter = 0;
            pipe_shift();
        }
        
        // --- 2. Bird Gravity/Physics Tick ---
        if (++gravity_counter >= BIRD_GRAVITY_DELAY) {
            gravity_counter = 0;
            
            // State transitions based on current state
            if (bird_state == BIRD_GOING_UP) {
                // After an UP press, hold the height for one tick (Stabilized)
                bird_state = BIRD_STABILIZED; 
            } else if (bird_state == BIRD_STABILIZED) {
                // After holding, start falling (or continue after jump hold)
                bird_state = BIRD_FALLING;
            }
            
            // Apply Gravity (only if falling)
            if (bird_state == BIRD_FALLING) {
                if (bird_vrow < VIRTUAL_ROWS - 2) { 
                    bird_vrow++;
                } else {
                    // Clamp bird to the floor
                    bird_vrow = VIRTUAL_ROWS - 2;
                }
            }
        }

        // --- 3. Update screen ---
        screen_update();
        
        // --- 4. Loop Delay (Game Speed) ---
        _delay_ms(1000); 
    }
}
