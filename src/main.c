#include <avr/io.h>        
#include <avr/interrupt.h> 
#include <stdint.h>        
#include <stdlib.h>        

#define CPU_CLOCK 16000000
#define PRESCALER 64
#define MAX_NOTE 100
#define TOLERANTA_ADC 25
#define NOISE_FLOOR 10

volatile uint32_t systicks = 0;
volatile uint16_t val_ADC_ISR[6];
volatile uint8_t canal_curent = 0;
volatile uint8_t flag_ADC = 0;

typedef enum  {
  CLAPA_IDLE,
  CLAPA_SETTLING,
  CLAPA_PLAYING
} StareClapa;

typedef enum {
  SEQ_FREE_PLAY,
  SEQ_RECORDING,
  SEQ_PLAYBACK
} StareSequencer;

typedef struct {
  StareClapa stare;
  int16_t val_prev;
  uint16_t frecventa_curenta;
  uint32_t timp_apasare;
} CanalSynth;

typedef struct {
  StareSequencer stare;
  uint16_t secventa[100];
  uint8_t index_final;
  uint8_t index_curent;
  uint16_t BPM;
  uint32_t ultimul_tick_ms;
} Sequencer;

const int16_t adc_calibrat[4] = {500, 674, 759, 811};
const int16_t adc_calibrat_control[3] = {492, 664, 753};

// 3 randuri (canalele 0, 1, 2) x 4 coloane (butoanele 0, 1, 2, 3)
const uint16_t frecventa_note[3][4] = {
  {262, 277, 294, 311}, // Canal 0: C4, C#4, D4, D#4
  {330, 349, 370, 392}, // Canal 1: E4, F4, F#4, G4 
  {415, 440, 466, 494}  // Canal 2: G#4, A4, A#4, B4
};

void timer1_init() 
{
  TCCR1A = 0;
  TCCR1B = 0;

  TCCR1A |= (1 << WGM11)   | (1 << COM1A1);
  TCCR1B |= (1 << CS11) | (1 << CS10) | (1 << WGM13) | (1 << WGM12) ;

  ICR1 = 0;
  OCR1A = 0;
}

void timer0_init() // numara 1 ms
{
  TCCR0A = 0;
  TCCR0B = 0;

  TCCR0A |= (1 << WGM01);
  TCCR0B |= (1 << CS01) | (1 << CS00);
  
  OCR0A = 249;
  TIMSK0 |= (1 << OCIE0A);
}

void ADC_init()
{
  ADMUX |= (1 << REFS0);
  ADCSRA |= (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0) | (1 << ADATE) | (1 << ADIE);
  ADCSRB |= (1 << ADTS1) | (1 << ADTS0);
}

void GPIO_init()
{
  DDRB = (1 << PB0) | (1 << PB1); // pinii 8 si 9 
}

uint16_t calculeaza_tc(int frecventa, int prescaler)
{
  return CPU_CLOCK / ((uint32_t)frecventa * prescaler) - 1;
}

ISR(TIMER0_COMPA_vect)

{
  systicks++;
}

ISR(ADC_vect)
{
  val_ADC_ISR[canal_curent] = ADC;

  canal_curent++;

  if(canal_curent > 5)
  {
    canal_curent = 0;
    flag_ADC = 1;
  }
  ADMUX = (ADMUX & 0xF0) | canal_curent;
}

int main()
{
  int16_t val_ADC[6];
  uint8_t note_active = 0;
  uint8_t octava_baza = 4;
  uint8_t octava_curenta = 4;
  int8_t diferenta_octave = 0;

  CanalSynth canale_synth[4];
  for(int i = 0 ; i < 4 ; i++)
  {
    canale_synth[i].stare = CLAPA_IDLE;
    canale_synth[i].val_prev = 0;
    canale_synth[i].frecventa_curenta = 0;
  }

  Sequencer seq;
  seq.BPM = 0;
  seq.index_final = 0;
  seq.index_curent = 0;
  seq.stare = SEQ_FREE_PLAY;
  for(int i = 0 ; i < 100; i++)
  {
    seq.secventa[i] = 0;
  }

  ADC_init();
  timer0_init();
  timer1_init();
  GPIO_init();
  sei();
  
  while(1)
  {
    if(flag_ADC == 1)
    {
      flag_ADC = 0;
      cli();
      for(int i = 0 ; i < 6; i++)
      {
        val_ADC[i] = val_ADC_ISR[i];
      }
      sei();
      seq.BPM = (val_ADC[4] * 200L) / 1023 + 40 ;

      for(int i = 0 ; i < 4 ;i ++)
      {
        switch (canale_synth[i].stare)
        {
          case CLAPA_IDLE:
          {
            if(val_ADC[i] > (NOISE_FLOOR + TOLERANTA_ADC))
            {
              if(seq.stare != SEQ_PLAYBACK || i == 3)
              { 
                canale_synth[i].stare = CLAPA_SETTLING;
                canale_synth[i].val_prev = val_ADC[i];
                cli();
                canale_synth[i].timp_apasare = systicks;
                sei();
              }
            }
            break;
          }
            

          case CLAPA_SETTLING:
          {
            uint32_t tick_curent;
              cli();
              tick_curent = systicks;
              sei();
            if(val_ADC[i] < NOISE_FLOOR + TOLERANTA_ADC)
            {
              canale_synth[i].stare = CLAPA_IDLE;
            }
              
              // abs(val_ADC[i] - canale_synth[i].val_prev) < TOLERANTA_ADC
            else if(tick_curent - canale_synth[i].timp_apasare > 20)
            {
              // var stare
              canale_synth[i].stare = CLAPA_PLAYING;
              

              // det frecventa
              uint16_t diferenta = 2000;
              uint8_t index = 0;
              uint16_t frecventa_curenta = 0;
              if(i < 3 )
              {
                for(int j = 0 ; j < 4 ; j++)
                {
                  if(abs(val_ADC[i] - adc_calibrat[j]) < diferenta)
                  {
                    diferenta = abs(val_ADC[i] - adc_calibrat[j]);
                    index = j;
                  }
                }
              }
              else
              {
                for(int j = 0 ; j < 3 ; j++) 
                {
                  if(abs(val_ADC[i] - adc_calibrat_control[j]) < diferenta)
                  {
                    diferenta = abs(val_ADC[i] - adc_calibrat_control[j]);
                    index = j;
                  }
                }
              }
              
              // det frecventa

              // procesare frecventa
              if(i < 3) // doar primele 3 canale ale ADC-ului contin note
              {
                note_active++;
                diferenta_octave = octava_curenta - octava_baza;
                frecventa_curenta = frecventa_note[i][index];
                if(diferenta_octave > 0)
                {
                  frecventa_curenta = frecventa_curenta << diferenta_octave;
                }
                else
                {
                  frecventa_curenta = frecventa_curenta >> (-diferenta_octave);
                }
                uint16_t top_val = calculeaza_tc(frecventa_curenta, PRESCALER);
                canale_synth[i].frecventa_curenta = frecventa_curenta;
                if(seq.stare == SEQ_RECORDING )
                {
                  if(seq.index_final < 100)
                  {
                    seq.secventa[seq.index_final] = canale_synth[i].frecventa_curenta;
                    seq.index_final++;
                  }
                }
                PORTB |= (1 << PB0);
                TCCR1A |= (1 << COM1A1);
                ICR1 = top_val;
                OCR1A = ((uint32_t)ICR1 * val_ADC[5]) / 1023;
              }
              // procesare frecventa
              else
              {
               
                if(index == 0 && octava_curenta < 7) // octave up
                {
                  octava_curenta++;
                }
                else if(index == 1 && octava_curenta > 1) // octave down
                {
                  octava_curenta--;
                }
                else if(index == 2) // schimbare stare sequencer 
                {
                  if(seq.stare == SEQ_FREE_PLAY)
                  {
                    seq.stare = SEQ_RECORDING;
                    seq.index_final = 0;
                  }
                    
                  else if(seq.stare == SEQ_RECORDING)
                  {
                    seq.stare = SEQ_PLAYBACK;
                    seq.index_curent = 0;     
                    note_active = 0;           
                    
                    canale_synth[0].stare = CLAPA_IDLE;
                    canale_synth[1].stare = CLAPA_IDLE;
                    canale_synth[2].stare = CLAPA_IDLE;

                    cli();
                    seq.ultimul_tick_ms = systicks;
                    sei();                  
                  }
                    
                  else if(seq.stare == SEQ_PLAYBACK)
                  {
                    seq.stare = SEQ_FREE_PLAY;

                    PORTB &= ~(1 << PB0);
                    TCCR1A &= ~(1 << COM1A1);
                    PORTB &= ~(1 << PB1);
                  }
                }
                canale_synth[i].stare = CLAPA_PLAYING;
              }
            }
            else
            {
              canale_synth[i].val_prev = val_ADC[i];
            }
            break;
          }
            

          case CLAPA_PLAYING:
          {
            if(val_ADC[i] < NOISE_FLOOR + TOLERANTA_ADC)
            {
              
              canale_synth[i].stare = CLAPA_IDLE;

              if(i < 3)
              {
                note_active--;
                 if(note_active > 0)
                 {
                  for(int j = 0; j < 3; j++)
                  {
                    if(canale_synth[j].stare == CLAPA_PLAYING)
                    {
                      ICR1 =  calculeaza_tc(canale_synth[j].frecventa_curenta, PRESCALER);
                      OCR1A = ((uint32_t)ICR1 * val_ADC[5]) / 1023;
                    }
                  }
                 }
                 else if(note_active == 0)
                 {
                  PORTB &= ~(1 << PB0);
                  TCCR1A &= ~(1 << COM1A1);
                  PORTB &= ~(1 << PB1);
                    //ICR1 = 0;
                  OCR1A = 0;
                  }
              }
            }
            break;
          }
        }
      }
    }
    if (TCCR1A & (1 << COM1A1)) 
    {
      OCR1A = ((uint32_t)ICR1 * val_ADC[5]) / 1023;
    }
    if(seq.stare == SEQ_PLAYBACK && seq.index_final >0)
    {
      if(seq.BPM < 40)
        seq.BPM = 40;
      
      uint32_t pas_ms = 60000UL / seq.BPM;
      uint32_t gate_ms = (pas_ms * 8) /10;

      uint32_t current_ticks;
      cli();
      current_ticks = systicks;
      sei();
      //PORTB &= ~(1 << PB0);
      if(current_ticks - seq.ultimul_tick_ms >= pas_ms)
      {
        seq.ultimul_tick_ms += pas_ms;
        uint16_t frecventa = seq.secventa[seq.index_curent];
        uint16_t top_val = calculeaza_tc(frecventa, PRESCALER);
        PORTB |= (1 << PB0);
        TCCR1A |= (1 << COM1A1);
        ICR1 = top_val;
        //OCR1A = ((uint32_t)ICR1 * val_ADC[5]) / 1023;
        seq.index_curent++;
        if(seq.index_curent >= seq.index_final)
        {
          seq.index_curent = 0;
        }
      }
      else if(current_ticks -  seq.ultimul_tick_ms >= gate_ms)
      {
        PORTB &= ~(1 << PB0);
        TCCR1A &= ~(1 << COM1A1); 
        PORTB &= ~(1 << PB1);    
      }
    }
  }
  return 0;
}
