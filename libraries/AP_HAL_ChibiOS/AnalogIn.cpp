/*
 * This file is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * Code by Andrew Tridgell and Siddharth Bharat Purohit
 */
#include <AP_HAL/AP_HAL.h>

#include "AnalogIn.h"

#if HAL_WITH_IO_MCU
#include <AP_IOMCU/AP_IOMCU.h>
extern AP_IOMCU iomcu;
#endif

#define ANLOGIN_DEBUGGING 0

// base voltage scaling for 12 bit 3.3V ADC
#define VOLTAGE_SCALING (3.3f/4096.0f)

#if ANLOGIN_DEBUGGING
 # define Debug(fmt, args ...)  do {printf("%s:%d: " fmt "\n", __FUNCTION__, __LINE__, ## args); } while(0)
#else
 # define Debug(fmt, args ...)
#endif

extern const AP_HAL::HAL& hal;

using namespace ChibiOS;

/*
  scaling table between ADC count and actual input voltage, to account
  for voltage dividers on the board. 
 */
const AnalogIn::pin_info AnalogIn::pin_config[] = HAL_ANALOG_PINS;

#define ADC_GRP1_NUM_CHANNELS   ARRAY_SIZE_SIMPLE(AnalogIn::pin_config)

// samples filled in by ADC DMA engine
adcsample_t AnalogIn::samples[ADC_DMA_BUF_DEPTH*ADC_GRP1_NUM_CHANNELS];
uint32_t AnalogIn::sample_sum[ADC_GRP1_NUM_CHANNELS];
uint32_t AnalogIn::sample_count;

AnalogSource::AnalogSource(int16_t pin, float initial_value) :
    _pin(pin),
    _value(initial_value),
    _value_ratiometric(initial_value),
    _latest_value(initial_value),
    _sum_count(0),
    _sum_value(0),
    _sum_ratiometric(0)
{
}


float AnalogSource::read_average() 
{
    if (_sum_count == 0) {
        return _value;
    }
    _value = _sum_value / _sum_count;
    _value_ratiometric = _sum_ratiometric / _sum_count;
    _sum_value = 0;
    _sum_ratiometric = 0;
    _sum_count = 0;
    return _value;
}

float AnalogSource::read_latest() 
{
    return _latest_value;
}

/*
  return scaling from ADC count to Volts
 */
float AnalogSource::_pin_scaler(void)
{
    float scaling = VOLTAGE_SCALING;
    for (uint8_t i=0; i<ADC_GRP1_NUM_CHANNELS; i++) {
        if (AnalogIn::pin_config[i].channel == _pin) {
            scaling = AnalogIn::pin_config[i].scaling;
            break;
        }
    }
    return scaling;
}

/*
  return voltage in Volts
 */
float AnalogSource::voltage_average()
{
    return _pin_scaler() * read_average();
}

/*
  return voltage in Volts, assuming a ratiometric sensor powered by
  the 5V rail
 */
float AnalogSource::voltage_average_ratiometric()
{
    voltage_average();
    return _pin_scaler() * _value_ratiometric;
}

/*
  return voltage in Volts
 */
float AnalogSource::voltage_latest()
{
    return _pin_scaler() * read_latest();
}

void AnalogSource::set_pin(uint8_t pin)
{
    if (_pin == pin) {
        return;
    }
    _pin = pin;
    _sum_value = 0;
    _sum_ratiometric = 0;
    _sum_count = 0;
    _latest_value = 0;
    _value = 0;
    _value_ratiometric = 0;
}

/*
  apply a reading in ADC counts
 */
void AnalogSource::_add_value(float v, float vcc5V)
{
    _latest_value = v;
    _sum_value += v;
    if (vcc5V < 3.0f) {
        _sum_ratiometric += v;
    } else {
        // this compensates for changes in the 5V rail relative to the
        // 3.3V reference used by the ADC.
        _sum_ratiometric += v * 5.0f / vcc5V;
    }
    _sum_count++;
    if (_sum_count == 254) {
        _sum_value /= 2;
        _sum_ratiometric /= 2;
        _sum_count /= 2;
    }
}


AnalogIn::AnalogIn() :
    _board_voltage(0),
    _servorail_voltage(0),
    _power_flags(0)
{
}

/*
  callback from ADC driver when sample buffer is filled
 */
void AnalogIn::adccallback(ADCDriver *adcp, adcsample_t *buffer, size_t n)
{
    if (buffer != &samples[0]) {
        return;
    }
    for (uint8_t i = 0; i < ADC_DMA_BUF_DEPTH; i++) {
        for (uint8_t j = 0; j < ADC_GRP1_NUM_CHANNELS; j++) { 
            sample_sum[j] += *buffer++;
        }
    }
    sample_count += ADC_DMA_BUF_DEPTH;
}

/*
  setup adc peripheral to capture samples with DMA into a buffer
 */
void AnalogIn::init()
{
    adcStart(&ADCD1, NULL);
    memset(&adcgrpcfg, 0, sizeof(adcgrpcfg));
    adcgrpcfg.circular = true;
    adcgrpcfg.num_channels = ADC_GRP1_NUM_CHANNELS;
    adcgrpcfg.end_cb = adccallback;
    adcgrpcfg.cr2 = ADC_CR2_SWSTART;
    adcgrpcfg.sqr1 = ADC_SQR1_NUM_CH(ADC_GRP1_NUM_CHANNELS);

    for (uint8_t i=0; i<ADC_GRP1_NUM_CHANNELS; i++) {
        uint8_t chan = pin_config[i].channel;
        // setup cycles per sample for the channel
        if (chan < 10) {
            adcgrpcfg.smpr2 |= ADC_SAMPLE_480 << (3*chan);
        } else {
            adcgrpcfg.smpr1 |= ADC_SAMPLE_480 << (3*(chan-10));
        }
        // setup channel sequence
        if (i < 6) {
            adcgrpcfg.sqr3 |= chan << (5*i);
        } else if (i < 12) {
            adcgrpcfg.sqr2 |= chan << (5*(i-6));
        } else {
            adcgrpcfg.sqr1 |= chan << (5*(i-12));
        }
    }
    adcStartConversion(&ADCD1, &adcgrpcfg, &samples[0], ADC_DMA_BUF_DEPTH); 
}

/*
  calculate average sample since last read for all channels
 */
void AnalogIn::read_adc(uint32_t *val)
{
    chSysLock();
    for (uint8_t i = 0; i < ADC_GRP1_NUM_CHANNELS; i++) {
        val[i] = sample_sum[i] / sample_count;
    }
    memset(sample_sum, 0, sizeof(sample_sum));
    sample_count = 0;
    chSysUnlock();
}

/*
  called at 1kHz
 */
void AnalogIn::_timer_tick(void)
{
    // read adc at 100Hz
    uint32_t now = AP_HAL::micros();
    uint32_t delta_t = now - _last_run;
    if (delta_t < 10000) {
        return;
    }
    _last_run = now;

    uint32_t buf_adc[ADC_GRP1_NUM_CHANNELS];

    /* read all channels available */
    read_adc(buf_adc);
    
    // match the incoming channels to the currently active pins
    for (uint8_t i=0; i < ADC_GRP1_NUM_CHANNELS; i++) {
#ifdef ANALOG_VCC_5V_PIN
        if (pin_config[i].channel == ANALOG_VCC_5V_PIN) {
            // record the Vcc value for later use in
            // voltage_average_ratiometric()
            _board_voltage = buf_adc[i] * pin_config[i].scaling;
        }
#endif
    }
    for (uint8_t i=0; i<ADC_GRP1_NUM_CHANNELS; i++) {
        Debug("chan %u value=%u\n",
              (unsigned)pin_config[i].channel,
              (unsigned)buf_adc[i]);
        for (uint8_t j=0; j < ADC_GRP1_NUM_CHANNELS; j++) {
            ChibiOS::AnalogSource *c = _channels[j];
            if (c != nullptr && pin_config[i].channel == c->_pin) {
                // add a value
                c->_add_value(buf_adc[i], _board_voltage);
            }
        }
    }

#if HAL_WITH_IO_MCU
    // now handle special inputs from IOMCU
    _servorail_voltage = iomcu.get_vservo();
#endif
}

AP_HAL::AnalogSource* AnalogIn::channel(int16_t pin) 
{
    for (uint8_t j=0; j<ANALOG_MAX_CHANNELS; j++) {
        if (_channels[j] == nullptr) {
            _channels[j] = new AnalogSource(pin, 0.0f);
            return _channels[j];
        }
    }
    hal.console->printf("Out of analog channels\n");
    return nullptr;
}

