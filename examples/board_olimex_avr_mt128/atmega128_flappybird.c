/**
 * Flappy Bird - Mini LCD Game
 * Complete game logic including collision detection, score, and state management.
 */

#undef F_CPU
#define F_CPU 16000000
#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega128");

#define __AVR_ATmega128__ 1
#include <avr/io.h>
#include <util/delay.h> // This is where _delay_ms() comes from!

// GENERAL INIT - USED BY ALMOST EVERYTHING ----------------------------------

static void port_init() {
    // Only CENTER button (PINA2) is used for input. PINA is pulled high internally.
    PORTA = 0b00011111; DDRA = 0b01000000; // buttons (only Center is used) & led
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
    // Use TCNT0 and TCNT1 for better entropy
    return (TCNT0 + TCNT1) % max;
}

// BUTTON HANDLING -----------------------------------------------------------

#define BUTTON_NONE     0
#define BUTTON_CENTER   1
static int button_accept = 1;

static int button_pressed() {
    // CENTER button (PIN A2 is active low)
    if (!(PINA & 0b00000100) && button_accept) { 
        button_accept = 0; 
        return BUTTON_CENTER;
    }
    return BUTTON_NONE;
}

static void button_unlock() {
    // Check if the CENTER button (PINA2) is released before re-enabling input
    // PINA2 is high when released
    if (PINA & 0b00000100)
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

// GAME STATE AND PHYSICS ----------------------------------------------------

#define COLS 16
#define VIRTUAL_ROWS 8
#define PIPE_GAP_SIZE 3
static unsigned char pipe_gaps[COLS]; // 9 means no pipe

// Bird State
#define BIRD_COL 3          // Bird is always drawn at column 3
#define BIRD_FALLING 0      // Bird is accelerating downwards
#define BIRD_STABILIZED 1   // Bird is hovering (no vertical movement)
#define BIRD_GOING_UP 2     // Bird is performing the single-step upward flap

static int bird_vrow;       // Bird's top virtual row (0 to VIRTUAL_ROWS - 2, max 6)
static int bird_state;
static int score;           // Player score
static int center_button_event; // NEW: Flag to hold button press until physics tick consumes it

// Game States
#define GAME_READY 0
#define GAME_RUNNING 1
#define GAME_OVER 2
static int game_state;

// Timing
#define PIPE_SHIFT_DELAY 5  // How many loops between column shifts (300ms)
#define PIPE_SPAWN_COL (COLS - 1) 
#define BIRD_GRAVITY_DELAY 7 // Bird physics tick every x game loops (x * 0.1s)

static void game_init() {
    for (int i = 0; i < COLS; ++i) {
        pipe_gaps[i] = 9; 
    }
    // Set initial bird position (forth virtual row, index 3) and state
    bird_vrow = 3; 
    bird_state = BIRD_FALLING;
    score = 0;
    center_button_event = 0; // Initialize the new event flag
}

/**
 * Shifts the entire pipe array one column to the left and spawns new pipes.
 * This also handles score increment when a pipe successfully passes the bird.
 */
static void pipe_shift() {
    // Check for score: If the pipe at BIRD_COL (3) is about to move off, 
    // and it's an actual pipe, the bird has passed it.
    if (pipe_gaps[BIRD_COL] != 9) {
        score++;
    }

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
        // Pipes must have enough space above/below, so gap_start must be between 1 and 4
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

/**
 * Checks for collision between the bird and any solid pipe segment at BIRD_COL.
 * Also checks against the virtual floor/ceiling boundaries (vrows 0 to 7).
 * Returns 1 on collision, 0 otherwise.
 */
static int check_collision() {
    // Check floor/ceiling collision
    // Bird occupies 2 vrows. VIRTUAL_ROWS - 2 = 6 (the highest possible safe top row index)
    if (bird_vrow < 0 || bird_vrow > VIRTUAL_ROWS - 2) {
        return 1; // Collision with ceiling (vrow < 0) or floor (vrow > 6)
    }
    
    int gap_start = pipe_gaps[BIRD_COL];
    
    // If no pipe at bird's column, no pipe collision
    if (gap_start == 9) {
        return 0; 
    }
    
    int gap_end = gap_start + PIPE_GAP_SIZE;
    
    // Collision with top pipe: top of bird is inside the pipe block
    if (bird_vrow < gap_start) {
        return 1; 
    }
    
    // Collision with bottom pipe: bottom of bird (vrow + 1) is inside the pipe block
    if (bird_vrow + 1 >= gap_end) { 
        return 1; 
    }

    return 0; // No collision
}


// CUSTOM CHARACTERS AND RENDERING -------------------------------------------

#define CHAR_SOLID              0
#define CHAR_TOP_HALF           1
#define CHAR_BOTTOM_HALF        2
#define CHAR_BIRD_BOTTOM_PIPE_TOP 3 // bird bottom + full top
#define CHAR_BIRD_TOP_PIPE_BOTTOM 4 // bird top + full bottom
#define CHAR_BIRD_TOP_EMPTY_BOTTOM 5 // bird top + empty bottom
#define CHAR_BIRD_BOTTOM_EMPTY_TOP 6 // bird bottom + empty top

#define CHARMAP_SIZE            7 

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
    int vrow_start = line * 4; 
    int gap_start = pipe_gaps[col];
    
    // --- BIRD DRAWING LOGIC (Priority in BIRD_COL) ---
    if (col == BIRD_COL) {
        
        // Quantize the bird's vrow position for drawing to ensure perfect alignment
        int bird_vrow_draw = (bird_vrow / 2) * 2; 

        // Check if the bird's 2-vrow range overlaps with this 5x8 character's 4-vrow range
        if (bird_vrow_draw + 1 >= vrow_start && bird_vrow_draw < vrow_start + 4) {

            // Bird is in this character cell. Now determine pipe background.
            int gap_end = gap_start + PIPE_GAP_SIZE;
            
            // Half 1 (Top 4 rows of char): Covers vrows vrow_start and vrow_start + 1 
            int is_half1_gap = (vrow_start < gap_end && vrow_start + 1 >= gap_start);
            
            // Half 2 (Bottom 4 rows of char): Covers vrows vrow_start + 2 and vrow_start + 3 
            int is_half2_gap = (vrow_start + 2 < gap_end && vrow_start + 3 >= gap_start);

            // Determine if pipe parts are solid, but only if a pipe exists (gap_start != 9)
            int half1_solid = (gap_start != 9) && !is_half1_gap;
            int half2_solid = (gap_start != 9) && !is_half2_gap;
            
            // Determine bird position within the character cell
            int vrow_offset = bird_vrow_draw - vrow_start; 
            
            // Bird is in the top half (vrow 0/1 or 4/5) of the 4-vrow cell.
            int bird_is_top_half_of_char_cell = (vrow_offset < 2);
            
            if (bird_is_top_half_of_char_cell) {
                if (half2_solid) {
                    return CHAR_BIRD_TOP_PIPE_BOTTOM; 
                } else {
                    return CHAR_BIRD_TOP_EMPTY_BOTTOM; 
                }
            } else {
                if (half1_solid) {
                    return CHAR_BIRD_BOTTOM_PIPE_TOP; 
                } else {
                    return CHAR_BIRD_BOTTOM_EMPTY_TOP; 
                }
            }
        }
    }
    
    // --- PIPE/EMPTY DRAWING (for all columns) ---
    
    if (gap_start == 9) {
        return ' '; // Column is empty
    }

    // Pipe logic:
    int gap_end = gap_start + PIPE_GAP_SIZE;
    int is_half1_gap = (vrow_start < gap_end && vrow_start + 1 >= gap_start);
    int is_half2_gap = (vrow_start + 2 < gap_end && vrow_start + 3 >= gap_start);

    int half1_solid = !is_half1_gap;
    int half2_solid = !is_half2_gap;

    if (half1_solid && half2_solid) return CHAR_SOLID;
    if (half1_solid) return CHAR_TOP_HALF;
    if (half2_solid) return CHAR_BOTTOM_HALF;
    
    return ' '; // Entire character is within the gap
}

/**
 * Converts score integer to a 4-digit string for display.
 */
static void score_to_string(char* buffer, int current_score) {
    buffer[4] = '\0';
    buffer[3] = (current_score % 10) + '0';
    current_score /= 10;
    buffer[2] = (current_score % 10) + '0';
    current_score /= 10;
    buffer[1] = (current_score % 10) + '0';
    current_score /= 10;
    buffer[0] = (current_score % 10) + '0';
}

static void screen_update() {
    char score_str[5] = "0000";
    char line2_buffer[COLS + 1];
    
    if (game_state == GAME_OVER) {
        lcd_send_line1("  GAME OVER!    ");
        
        score_to_string(score_str, score);
        // Display score on line 2 during Game Over
        line2_buffer[0] = 'S'; line2_buffer[1] = 'c'; line2_buffer[2] = 'o'; 
        line2_buffer[3] = 'r'; line2_buffer[4] = 'e'; line2_buffer[5] = ':'; line2_buffer[6] = ' ';
        for(int i = 0; i < 4; ++i) line2_buffer[7+i] = score_str[i];
        line2_buffer[11] = ' '; line2_buffer[12] = 'R'; line2_buffer[13] = 'E'; line2_buffer[14] = 'S'; line2_buffer[15] = '\0';

        lcd_send_line2(line2_buffer);

    } else if (game_state == GAME_READY) {
        lcd_send_line1(" Flappy C Test ");
        lcd_send_line2(" Press Center   ");

    } else { // GAME_RUNNING
        // Line 1 (vrows 0-3) - Draw game world
        lcd_send_command(DD_RAM_ADDR);
        for (int col = 0; col < COLS; ++col) {
            lcd_send_data(get_char_for_col(col, 0));
        }

        // Line 2 (vrows 4-7) - Draw game world AND embed the score in the empty space
        lcd_send_command(DD_RAM_ADDR2);
        for (int col = 0; col < COLS; ++col) {
            line2_buffer[col] = get_char_for_col(col, 1);
        }
        line2_buffer[COLS] = '\0';

        // Overwrite score at the top right corner of the second line
        score_to_string(score_str, score);
        line2_buffer[12] = score_str[1];
        line2_buffer[13] = score_str[2];
        line2_buffer[14] = score_str[3];
        line2_buffer[15] = '\0';
        
        lcd_send_line2(line2_buffer);
    }
}


// THE MAIN LOOP =============================================================

int main() {
    port_init();
    lcd_init();
    chars_init();
    rnd_init();

    game_init(); 
    game_state = GAME_READY;
    
    int pipe_shift_counter = 0;
    int gravity_counter = 0;

    // Main Game Loop (State Machine)
    while (1) {
        
        // --- 0. Handle Input & State Transition ---
        int input = button_pressed();
        button_unlock(); 
        
        if (game_state == GAME_READY) {
            if (input == BUTTON_CENTER) {
                game_state = GAME_RUNNING;
                // Clear the display for the start of the game
                lcd_send_command(CLR_DISP); 
            }
        } else if (game_state == GAME_OVER) {
            if (input == BUTTON_CENTER) {
                game_init(); // Reset all game data (pipes, bird, score)
                game_state = GAME_RUNNING;
                pipe_shift_counter = 0; // Reset timers
                gravity_counter = 0;
                // Clear the display for the start of the game
                lcd_send_command(CLR_DISP); 
            }
        }
        
        // --- 1. Game Running Logic ---
        if (game_state == GAME_RUNNING) {
            
            // Capture button press event for the delayed physics tick
            if (input == BUTTON_CENTER) {
                center_button_event = 1;
            }
            
            // Pipe Movement (Column Shift)
            if (++pipe_shift_counter >= PIPE_SHIFT_DELAY) {
                pipe_shift_counter = 0;
                pipe_shift();
            }
            
            // Bird Gravity/Physics Tick (State Machine Logic)
            if (++gravity_counter >= BIRD_GRAVITY_DELAY) {
                gravity_counter = 0;
                
                // Read and immediately consume the persistent event flag
                int event_happened = center_button_event;
                center_button_event = 0; 
                
                if (bird_state == BIRD_FALLING) {
                    if (event_happened) {
                        // Rule 1: FALLING + Button -> STABILIZED (Hover one tick)
                        bird_state = BIRD_STABILIZED;
                    } else {
                        // Default: FALLING + No Button -> Keep falling (Apply gravity)
                        if (bird_vrow < VIRTUAL_ROWS - 2) { 
                            bird_vrow++; 
                        }
                        // State remains BIRD_FALLING
                    }
                } 
                else if (bird_state == BIRD_STABILIZED) {
                    if (event_happened) {
                        // Rule 2: STABILIZED + Button -> GOING_UP (Apply Flap)
                        if (bird_vrow > 0) {
                            bird_vrow--; 
                        }
                        bird_state = BIRD_GOING_UP;
                    } else {
                        // Rule 4: STABILIZED + No Button -> FALLING
                        bird_state = BIRD_FALLING;
                    }
                } 
                else if (bird_state == BIRD_GOING_UP) {
                    // Rule 3: GOING_UP -> STABILIZED (Input is irrelevant after the flap)
                    bird_state = BIRD_STABILIZED;
                }
            }

            // --- 2. Collision Check ---
            if (check_collision()) {
                game_state = GAME_OVER;
            }
        }
        
        // --- 3. Update screen ---
        screen_update();
        
        // --- 4. Loop Delay (Game Speed) ---
        _delay_ms(300); 
    }
}