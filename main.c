#include <dos.h>
#include <string.h>
#include <ctype.h>
#include <graph.h>

typedef unsigned char u8;
typedef unsigned short u16;

// Key codes
#define KEY_F1 0x3B
#define KEY_F2 0x3C
#define KEY_F3 0x3D
#define KEY_F4 0x3E
#define KEY_F5 0x3F
#define KEY_F6 0x40
#define KEY_F7 0x41
#define KEY_F8 0x42
#define KEY_F9 0x43
#define KEY_F10 0x44
#define KEY_ESC 0x01
#define KEY_ENTER 0x1C
#define KEY_TAB 0x0F
#define KEY_BACKSPACE 0x0E

typedef struct {
    u8 ascii;
    u8 scan;
} KeyPress;

KeyPress last_kbd;

void wait_for_key()
{
    union REGS regs;
    
    // Wait for key
    regs.h.ah = 0x00;
    int86(0x16, &regs, &regs);
    
    last_kbd.ascii = regs.h.al;
    last_kbd.scan = regs.h.ah;
}

// Video memory segments
#define VIDEO_CGA 0xB800
#define VIDEO_MDA 0xB000

u16 video_segment = VIDEO_CGA;

#define SCREEN_COLS 80
#define SCREEN_ROWS 25

// Port status
typedef enum {
    PORT_UNKNOWN,
    PORT_EMPTY,      // reads 0xFF
    PORT_ACTIVE,     // reads something else
    PORT_FORBIDDEN   // system port, don't touch
} PortStatus;

// Port info structure
struct PortInfo {
    u16 address;
    PortStatus status;
    u8 last_read;
};

// Input field structure
struct InputField {
    u8 x, y;
    char* description;
    char data[16];
    u8 field_length; //max 16
};

// Screen IDs
typedef enum {
    SCREEN_INPUT,
    SCREEN_VISUALIZE,
	SCREEN_SETTINGS,
	
	SCREEN_COUNT
} ScreenID;

// Screen structure
struct Screen {
    ScreenID id;
    char* title;
	char key; //use KEY_ macros
};

// Screen definitions
struct Screen screens[] = {
    {SCREEN_INPUT, "IOPROBE.EXE - ISA PORT SCANNER v1.0", KEY_F1},	
    {SCREEN_VISUALIZE, "PORT VISUALIZATION", KEY_F2},
    {SCREEN_SETTINGS, "SETTINGS - PRESS ESC TO QUIT", KEY_F10}
};

// Visualization config
struct VisConfig {
    u8 grid_x, grid_y;      // Top-left of grid
    u8 cols, rows;          // Grid dimensions
    u16 start_port;
    u16 end_port;
    char char_empty;
    char char_active;
    char char_forbidden;
};

// Global state
struct {
    ScreenID current_screen;
    u8 active_field;
    struct PortInfo ports[1024];  // 0x000-0x3FF
    struct VisConfig vis;
} state;

// Field definitions
struct InputField fields[] = {
    {15, 5, "Start Port:", "0000", 4},
    {15, 6, "End Port:  ", "03FF", 4}
};

#define NUM_FIELDS (sizeof(fields) / sizeof(fields[0]))

// Forbidden port ranges
struct PortRange {
    u16 start;
    u16 end;
    char* description;
};

struct PortRange forbidden_ranges[] = {
    {0x000, 0x01F, "DMA Controller"},
    {0x020, 0x021, "PIC Master"},
    {0x040, 0x043, "PIT Timer"},
    {0x060, 0x064, "Keyboard"},
    {0x070, 0x071, "RTC/CMOS"},
    {0x080, 0x09F, "DMA Page"},
    {0x0A0, 0x0A1, "PIC Slave"},
    {0x0C0, 0x0DF, "DMA #2"},
    {0x0F0, 0x0FF, "FPU"},
    {0x170, 0x177, "IDE Secondary"},
    {0x1F0, 0x1F7, "IDE Primary"},
    {0x370, 0x377, "Floppy Secondary"},
    {0x378, 0x37F, "COM2"},
    {0x3B0, 0x3BF, "MDA/Hercules"},
    {0x3C0, 0x3CF, "VGA"},
    {0x3D0, 0x3DF, "CGA/VGA"},
    {0x3F0, 0x3F7, "Floppy Primary"},
    {0x3F8, 0x3FF, "COM1"}
};

#define NUM_FORBIDDEN (sizeof(forbidden_ranges) / sizeof(forbidden_ranges[0]))

// ============================================================================
// VIDEO FUNCTIONS
// ============================================================================

void write_char_at(u8 x, u8 y, char ch, u8 attr)
{
    u16 far* vmem = MK_FP(video_segment, 0);
    u16 offset = (y * SCREEN_COLS) + x;
    vmem[offset] = (attr << 8) | ch;
}

void write_string_at(u8 x, u8 y, char* str, u8 attr)
{
    while (*str) {
        write_char_at(x++, y, *str++, attr);
    }
}

void write_string_centered(u8 y, char* str, u8 attr)
{
	u8 x = (SCREEN_COLS-strlen(str))/2;
    while (*str) {
        write_char_at(x++, y, *str++, attr);
    }
}

void clear_screen()
{
    u16 far* vmem = MK_FP(video_segment, 0);
    for (u16 i = 0; i < SCREEN_COLS * SCREEN_ROWS; i++) {
        vmem[i] = 0x0720;  // Space with gray on black
    }
}

void draw_box(u8 x, u8 y, u8 w, u8 h, u8 attr)
{
    // Corners and edges
    write_char_at(x, y, 218, attr);  // ┌
    write_char_at(x + w - 1, y, 191, attr);  // ┐
    write_char_at(x, y + h - 1, 192, attr);  // └
    write_char_at(x + w - 1, y + h - 1, 217, attr);  // ┘
    
    // Horizontal lines
    for (u8 i = 1; i < w - 1; i++) {
        write_char_at(x + i, y, 196, attr);  // ─
        write_char_at(x + i, y + h - 1, 196, attr);
    }
    
    // Vertical lines
    for (u8 i = 1; i < h - 1; i++) {
        write_char_at(x, y + i, 179, attr);  // │
        write_char_at(x + w - 1, y + i, 179, attr);
    }
}

// ============================================================================
// PORT FUNCTIONS
// ============================================================================

int is_port_forbidden(u16 port)
{
    for (u8 i = 0; i < NUM_FORBIDDEN; i++) {
        if (port >= forbidden_ranges[i].start && 
            port <= forbidden_ranges[i].end) {
            return 1;
        }
    }
    return 0;
}

u8 read_port_safe(u16 port)
{
    if (is_port_forbidden(port)) {
        return 0;
    }
    return inp(port);
}

void scan_ports()
{
    u16 start = state.vis.start_port;
    u16 end = state.vis.end_port;
    
    for (u16 port = start; port <= end; port++) {
        u16 idx = port - start;
        
        state.ports[idx].address = port;
        
        if (is_port_forbidden(port)) {
            state.ports[idx].status = PORT_FORBIDDEN;
            state.ports[idx].last_read = 0;
        } else {
            state.ports[idx].last_read = read_port_safe(port);
            if (state.ports[idx].last_read == 0xFF) {
                state.ports[idx].status = PORT_EMPTY;
            } else {
                state.ports[idx].status = PORT_ACTIVE;
            }
        }
    }
}

// ============================================================================
// INPUT SCREEN
// ============================================================================

const char*const hexnum = "0123456789abcdef";

void sprintf_hex_short(char* rowname, u16 number)
{
	rowname[0] = hexnum[(number>>12)&0xF];
	rowname[1] = hexnum[(number>>8)&0xF];
	rowname[2] = hexnum[(number>>4)&0xF];
	rowname[3] = hexnum[(number>>0)&0xF];
	rowname[4] = 0;
}

void draw_field(struct InputField* field, u8 is_active)
{
    u8 attr = is_active ? 0x70 : 0x07;  // Inverted if active
    u8 x = field->x - strlen(field->description) - 1;
    
    write_string_at(x, field->y, field->description, 0x07);
    
    // Draw field box
    write_char_at(field->x, field->y, '[', 0x07);
    write_string_at(field->x + 1, field->y, field->data, attr);
    
    // Pad with spaces
    u8 len = strlen(field->data);
    for (u8 i = len; i < field->field_length; i++)
	{
        write_char_at(field->x + 1 + i, field->y, ' ', attr);
    }
    
    write_char_at(field->x + 1 + field->field_length, field->y, ']', 0x07);
}

void draw_input_screen()
{
    // Draw all fields
    for (u8 i = 0; i < NUM_FIELDS; i++)
	{
        draw_field(&fields[i], i == state.active_field);
    }
    
    // Instructions
    write_string_at(2, SCREEN_ROWS-1, "TAB: Next Field  ENTER: Scan  F2: Visualize  F10: Settings", 0x07);
}

int is_hex_digit(char value)
{
	return (value >= '0' && value <= '9')
	||(value >= 'A' && value <= 'F')
	||(value >= 'A' && value <= 'f');
}

void edit_field(struct InputField* field)
{
    u8 pos = strlen(field->data);
	
	if (last_kbd.scan == KEY_BACKSPACE) // Backspace
	{
		if (pos > 0) 
		{
			field->data[pos] = '\0';
		}
	}
	else if (is_hex_digit(last_kbd.ascii)) 
	{
		if (pos >= field->field_length)
		{
			pos = 0;
		}
		field->data[pos] = toupper(last_kbd.ascii);
		field->data[pos + 1] = '\0';
	}
}

void input_screen_loop()
{
	if (last_kbd.scan == KEY_ENTER) {  // Enter - scan
		scan_ports();
		state.current_screen = SCREEN_VISUALIZE;
	}
	else if (last_kbd.scan == KEY_TAB) {  // Tab
		state.active_field = (state.active_field + 1) % NUM_FIELDS;
	}
    edit_field(&fields[state.active_field]);
	draw_input_screen();
}

// ============================================================================
// VISUALIZATION SCREEN
// ============================================================================

void draw_vis_screen()
{
	// Legend
    write_string_at(2, 4, "Legend:", 0x0F);
    write_char_at(10, 4, state.vis.char_empty, 0x08);
    write_string_at(12, 4, "Empty (0xFF)", 0x07);
    write_char_at(26, 4, state.vis.char_active, 0x0A);
    write_string_at(28, 4, "Active", 0x07);
    write_char_at(36, 4, state.vis.char_forbidden, 0x0C);
    write_string_at(38, 4, "Forbidden", 0x07);
    
    // Draw grid
    for (u8 row = 0; row < state.vis.rows; row++)
	{
		char rowname[5] = {0};
		
		sprintf_hex_short(rowname, row*state.vis.cols+state.vis.start_port);
		write_string_at(state.vis.grid_x-5, state.vis.grid_y+row, rowname, 0x07);
		
        for (u8 col = 0; col < state.vis.cols; col++)
		{
            u16 port_idx = row * state.vis.cols + col;
            
			struct PortInfo *port;
            port = &state.ports[port_idx];
			
			u8 attr = 0;
			char ch = 0;
            
            switch (port->status)
			{
                case PORT_EMPTY:
                    ch = state.vis.char_empty;
                    attr = 0x08;  // Dark gray
                    break;
                case PORT_ACTIVE:
                    ch = state.vis.char_active;
                    attr = 0x0A;  // Bright green
                    break;
                case PORT_FORBIDDEN:
                    ch = state.vis.char_forbidden;
                    attr = 0x0C;  // Bright red
                    break;
                default:
                    ch = '?';
                    attr = 0x07;
            }
            
            write_char_at(state.vis.grid_x + col, 
                         state.vis.grid_y + row, 
                         ch, attr);
        }
    }
    
    // Port range display
    write_string_at(2, SCREEN_ROWS-2, "Range: 0x", 0x07);
    write_string_at(11, SCREEN_ROWS-2, fields[0].data, 0x0F);
    write_string_at(15, SCREEN_ROWS-2, " - 0x", 0x07);
    write_string_at(20, SCREEN_ROWS-2, fields[1].data, 0x0F);
    
    // Instructions
    write_string_at(35, SCREEN_ROWS-1, "F1: Input  F10: Settings  R: Rescan", 0x07);
}

void vis_screen_loop()
{
	if (last_kbd.ascii == 'r' || last_kbd.ascii == 'R')
	{
		scan_ports();
	}
    draw_vis_screen();
}

// ============================================================================
// MAIN
// ============================================================================

u16 hex_to_u16(char* hex)
{
    u16 result = 0;
    while (*hex)
	{
        result <<= 4;
		
        if (*hex >= '0' && *hex <= '9') {
            result += *hex - '0';
        } else if (*hex >= 'A' && *hex <= 'F') {
            result += *hex - ('A' - 10);
        }
        hex++;
    }
    return result;
}

void init_state()
{
    state.current_screen = SCREEN_INPUT;
    state.active_field = 0;
    
    // Visualization config
    state.vis.grid_x = 8;
    state.vis.grid_y = 6;
    state.vis.cols = 64;
    state.vis.rows = 16;
    state.vis.char_empty = 250;      // ·
    state.vis.char_active = 219;     // █
    state.vis.char_forbidden = 177;  // ▒
    
    // Will be updated from input fields
    state.vis.start_port = 0x000;
    state.vis.end_port = 0x3FF;
}

int main()
{
	_setvideomode(_TEXTC80);
    
    init_state();
    input_screen_loop();
	
    while (1)
	{
        // Update port range from input fields
		if (strlen(fields[0].data) == fields[0].field_length)
		{
			state.vis.start_port = hex_to_u16(fields[0].data);
		}
		if (strlen(fields[1].data) == fields[1].field_length)
		{
			state.vis.end_port = hex_to_u16(fields[1].data);
		}
		
		for(int screen_i=0; screen_i<SCREEN_COUNT; ++screen_i)
		{
			if (last_kbd.scan == screens[screen_i].key)
			{
				state.current_screen = screens[screen_i].id;
			}
		}

		clear_screen();
		
		//debug info
		/*write_char_at(0, 23, hexnum[last_kbd.scan>>4], 0x07);
		write_char_at(1, 23, hexnum[last_kbd.scan&0x0F], 0x07);
		write_char_at(2, 23, hexnum[last_kbd.ascii>>4], 0x07);
		write_char_at(3, 23, hexnum[last_kbd.ascii&0x0F], 0x07);*/
		
		write_string_centered(1, screens[state.current_screen].title, 0x0F);
		draw_box(0, 0, SCREEN_COLS, 3, 0x07);
        
        switch (state.current_screen)
		{
            case SCREEN_INPUT:
                input_screen_loop();
                break;
                
            case SCREEN_VISUALIZE:
                vis_screen_loop();
                break;
                
            default:
                break;
        }
		        
        if (state.current_screen == SCREEN_SETTINGS && last_kbd.scan == KEY_ESC)
		{
            break;
        }
		wait_for_key();
    }
    
    clear_screen();
    return 0;
}
//28240 before sprintf change
//23688 after sprintf change
//25032 after refactor
//24768 after optimization flags -s -os
//24756 after removing "is extended"
//24534 after writing is_hex_digit myself
//24412 just before release
