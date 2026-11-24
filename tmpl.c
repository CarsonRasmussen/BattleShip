/** !
 * Battleship implementation
 *
 * @file battleship.c
 *
 * @author Jacob Smith
 */

// headers
#include <string.h>
#include "tmpl.h"

// board configuration -- change these if your wiring differs
#define SWITCH_IDR  ( GPIOC->IDR )// register the slider switches
#define SW_LAYER_MASK ( 1u << 0 )// slider switch: 0 horizontal segments, 1 vertical pqa
#define BUTTON_PIN  ( 10 ) // PC10 confirm / place / fire
#define BUTTON_DOWN_LEVEL ( 0 ) // pin level while a pushbutton is held
#define DEBOUNCE_MS  ( 20 ) // pushbutton debounce time
#define SCROLL_MS ( 200 ) // marquee scroll speed (ms per character)
#define POT_X ( 0 ) //cursor column
#define POT_Y ( 1 )//cursor row

// game rules
#define SINGLE_BOATS ( 3 )
#define DOUBLE_BOATS ( 2 )
#define MESSAGE_MAX ( 32 )

// forward declarations
static void poll_switches ( void );
static void poll_potentiometers ( void );
static void store_message ( const char *p_message );

static fn_state *state_start;
static fn_state *state_p1_place;
static fn_state *state_p2_place;
static fn_state *state_p1_turn;
static fn_state *state_p2_turn;
static fn_state *state_result;

// data
static fn_state *_pfn_states[] =
{
	[STATE_START]    = state_start,
	[STATE_P1_PLACE] = state_p1_place,
	[STATE_P2_PLACE] = state_p2_place,
	[STATE_P1_TURN]  = state_p1_turn,
	[STATE_P2_TURN]  = state_p2_turn,
	[STATE_RESULT]   = state_result,
};

struct
{
	unsigned short sw;     // the switches (16 switches -> 16 bits -> short)
	unsigned short ld;     // the led's (16 leds -> 16 bits -> short)
	bool pc10;             // true IF PC10 is down ELSE false
	bool pc11;             // true IF PC11 is down ELSE false
	unsigned short _pa[3]; // raw potentiometer values (3 potentiometers -> 3 values -> array of 3 shorts)
	unsigned int time;     // the time (in ms) since the board started (incremented by timer interrupt)
} 
device_state =
{
	.sw   = 0,
	.ld   = 0,
	.pc10 = false,
	.pc11 = false,
	._pa  = 
	{ 
		0, // <- PA1
		0, // <- PA2
		0, // <- PA3
	},
};

// player data 
player players[PLAYER_QUANTITY]  =
{

	// player A's data
	[PLAYER_A] =
	{

		// player A's maps
		.maps =
		{

			// player A's ship map is empty when the game starts
			[SHIP] =
			{
				.horizontal = 
				{ 
					{ STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, },
					{ STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, },
					{ STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, },
				},
				.vertical =
				{
					{ STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, },
					{ STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, STILL, },
				},
			},

			// player A's shot map is empty when the game starts
			[SHOT] =
			{
				.horizontal = { 0 },
				.vertical   = { 0 },
			}
		}
		
	},

	// Player B's data
	[PLAYER_B] =
	{

		// player B's maps
		.maps =
		{

			// player B's ship map is empty when the game starts
			[SHIP] =
			{
				.horizontal = { 0 },
				.vertical   = { 0 },
			},

			// player B's shot map is empty when the game starts
			[SHOT] =
			{
				.horizontal = { 0 },
				.vertical   = { 0 },
			}
		},
	},
};

extern void input_poll ( void )
{

	// poll the switches
	poll_switches();

	// poll the potentiometers
	poll_potentiometers();

	// done
	return;
}

// returns the new (debounced) level of a pushbutton
static bool debounce ( debounce_t *p, bool sample, bool level )
{
	if ( sample != p->raw )
	{
		p->raw = sample;
		p->t   = device_state.time;
	}
	else if ( sample != level && ( device_state.time - p->t ) >= DEBOUNCE_MS )
		level = sample;

	return level;
}

static void poll_switches ( void )
{

	// static data
	static debounce_t db10, db11;

	// initialized data
	bool pc10, pc11;

	// store the state of the switches in device_state.sw
	device_state.sw = (unsigned short) SWITCH_IDR;

	// the pushbuttons
	pc10 = debounce( &db10, ( ( GPIOC->IDR >> BUTTON_PIN  ) & 1 ) == BUTTON_DOWN_LEVEL, device_state.pc10 );
	pc11 = debounce( &db11, ( ( GPIOC->IDR >> BUTTON2_PIN ) & 1 ) == BUTTON_DOWN_LEVEL, device_state.pc11 );

	// remember each new press of PC10 until the game logic consumes it
	if ( pc10 && !device_state.pc10 )
		pc10_edge = true;

	device_state.pc10 = pc10;
	device_state.pc11 = pc11;

	// done
	return;
}

// one-time ADC1 setup for PA1..PA3
static void adc_init ( void )
{
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
	RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

	// PA1, PA2, PA3 -> analog mode
	GPIOA->MODER |= ( 3u << ( 1 * 2 ) ) | ( 3u << ( 2 * 2 ) ) | ( 3u << ( 3 * 2 ) );

	// ADC clock = PCLK2 / 4 (the F407 ADC must stay at or below 36 MHz)
	ADC->CCR = ( ADC->CCR & ~ADC_CCR_ADCPRE ) | ADC_CCR_ADCPRE_0;

	// long sample time on channels 1, 2, 3
	ADC1->SMPR2 |= ( 7u << 3 ) | ( 7u << 6 ) | ( 7u << 9 );

	// turn the ADC on
	ADC1->CR2 |= ADC_CR2_ADON;
}

static unsigned short adc_read ( unsigned int channel )
{

	// initialized data
	unsigned int timeout = 20000;

	// one conversion, on the requested channel
	ADC1->SQR3 = channel;
	ADC1->CR2 |= ADC_CR2_SWSTART;

	// wait for the end of conversion (bounded so we can never hang the interrupt)
	while ( !( ADC1->SR & ADC_SR_EOC ) && timeout-- );

	// reading DR clears EOC
	return (unsigned short) ADC1->DR;
}

static void poll_potentiometers ( void )
{

	// static data
	static bool initialized = false;

	if ( !initialized )
	{
		adc_init();
		initialized = true;
	}

	// iterate through each potentiometer and store the value in the corresponding index of device_state._pa
	for ( unsigned int i = 0; i < 3; i++ )
		device_state._pa[i] = adc_read( i + 1 ); // PA1 -> ADC channel 1, etc.

	// done
	return;
}

extern void timer_step ( void )
{


	// increment the time
	device_state.time++;

	// done
	return;
}

extern void draw_board ( map _map )
{

	// initialized data
	unsigned char _board[8] = { 0 };
	bool should_dim   = (device_state.time % 10 < 5);
	bool should_blink = ( (device_state.time / 250) % 2 == 0 );

	// draw the horizontal segments
	for (size_t i = 0; i < 8; i++)
	{

		// set the top segment based on the horizontal[0] map
		switch(_map.horizontal[0][i])
		{
			case STILL:
				_board[i] |= 0;
				break;
			case HIT:
				_board[i] |= SEG7_TOP;
				break;
			case MISS:
				_board[i] |= should_dim ? SEG7_TOP : 0;
				break;
			case CURSOR:
				_board[i] |= should_blink ? SEG7_TOP : 0;
				break;
		}

		switch(_map.horizontal[1][i])
		{
			case STILL:
				_board[i] |= 0;
				break;
			case HIT:
				_board[i] |= SEG7_MIDDLE;
				break;
			case MISS:
				_board[i] |= should_dim ? SEG7_MIDDLE : 0;
				break;
			case CURSOR:
				_board[i] |= should_blink ? SEG7_MIDDLE : 0;
				break;
		}

		switch(_map.horizontal[2][i])
		{
			case STILL:
				_board[i] |= 0;
				break;
			case HIT:
				_board[i] |= SEG7_BOTTOM;
				break;
			case MISS:
				_board[i] |= should_dim ? SEG7_BOTTOM : 0;
				break;
			case CURSOR:
				_board[i] |= should_blink ? SEG7_BOTTOM : 0;
				break;
		}
	}

	// draw the vertical segments
	for (size_t i = 0; i < 8; i++)
	{


		switch(_map.vertical[0][2 * i])
		{
			case STILL:
				_board[i] |= 0;
				break;
			case HIT:
				_board[i] |= SEG7_TOP_LEFT;
				break;
			case MISS:
				_board[i] |= should_dim ? SEG7_TOP_LEFT : 0;
				break;
			case CURSOR:
				_board[i] |= should_blink ? SEG7_TOP_LEFT : 0;
				break;
		}


		switch(_map.vertical[0][2 * i + 1])
		{
			case STILL:
				_board[i] |= 0;
				break;
			case HIT:
				_board[i] |= SEG7_TOP_RIGHT;
				break;
			case MISS:
				_board[i] |= should_dim ? SEG7_TOP_RIGHT : 0;
				break;
			case CURSOR:
				_board[i] |= should_blink ? SEG7_TOP_RIGHT : 0;
				break;
		}


		switch(_map.vertical[1][2 * i])
		{
			case STILL:
				_board[i] |= 0;
				break;
			case HIT:
				_board[i] |= SEG7_BOTTOM_LEFT;
				break;
			case MISS:
				_board[i] |= should_dim ? SEG7_BOTTOM_LEFT : 0;
				break;
			case CURSOR:
				_board[i] |= should_blink ? SEG7_BOTTOM_LEFT : 0;
				break;
		}

		switch(_map.vertical[1][2 * i + 1])
		{
			case STILL:
				_board[i] |= 0;
				break;
			case HIT:
				_board[i] |= SEG7_BOTTOM_RIGHT;
				break;
			case MISS:
				_board[i] |= should_dim ? SEG7_BOTTOM_RIGHT : 0;
				break;
			case CURSOR:
				_board[i] |= should_blink ? SEG7_BOTTOM_RIGHT : 0;
				break;
		}

	}

	// write the board array to the seven segment display
	for(int i = 0; i < 8; i++)
		GPIOE->ODR = (0XFF00 | ~_board[i]) & ~(1<<((7-i)+8)),
		GPIOE->ODR |= 0xFF00;

	// done
	return;
}

extern void game_logic ( void )
{

	// static data
	static int state = STATE_START;

	// update
	_pfn_states[state]();

	// state transition
	switch (state)
	{
		case STATE_START:
			if ( device_state.pc10 )
				state = STATE_P1_PLACE;
			break;
		
		// player 1 places boats
		case STATE_P1_PLACE:

			// transition
			if ( 0 ) 
				state = STATE_P2_PLACE;
			
			break;
		
		// player 2 places boats
		case STATE_P2_PLACE:

			// transition
			if ( 0 ) 
				state = STATE_P2_PLACE;

			break;
		
		// player 1 takes a shot
		case STATE_P1_TURN:

			// transition
			if ( 0 ) 
				state = STATE_P2_TURN;
			else if ( 0 )
				state = STATE_RESULT;

			break;
		
		// player 2 takes a shot
		case STATE_P2_TURN:

			// transition
			if ( 0 ) 
				state = STATE_P1_TURN;
			else 
				state = STATE_RESULT;

			break;

		// game over
		case STATE_RESULT:
			if ( device_state.pc10 )
				state = STATE_START;
			break;

		// unknown state -> reset
		default: 
			state = STATE_START; 
			break;
	}
	
	// done
	return;
}

void state_p1_turn ( void ) 
{
	turn_step( PLAYER_A );
	return;
}

void state_p2_turn ( void ) 
{
	turn_step( PLAYER_B );
	return;
}

void state_result ( void ) 
{

	if ( state_fresh )
	{
		state_fresh = false;

		// declare the winner until someone presses PC10
		announce( winner == PLAYER_A ? "P1 WINS" : "P2 WINS", true );
		sound_win();
	}

	// PC10 goes back to the title screen
	if ( button_pressed() )
		state_done = true;

	// done
	return;
}