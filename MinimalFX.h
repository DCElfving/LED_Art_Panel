#ifndef MinimalFX_h
#define MinimalFX_h

#include <Arduino.h>

#define NEO_RGB     0x00
#define NEO_KHZ800  0x00
#define FX_MODE_STATIC 0
#define FX_MODE_CUSTOM 100 // Arbitrary start for custom

class WS2812FX {
public:
    typedef uint16_t (*mode_ptr)(void);
    typedef void (*show_callback)(void);

    WS2812FX(uint16_t num_leds, int pin, uint8_t type) : _mode_last_call_time(0), _mode_delay(0), _custom_show(NULL), _custom_mode(NULL) {
        _num_leds = num_leds;
        _mode_ptr = NULL;
        _mode_index = 0;
        _speed = 1000;
        _brightness = 255;
        _color = 0;
        _running = false;
        
        _led_buffer = (uint32_t*)malloc(num_leds * sizeof(uint32_t));
        if(_led_buffer) memset(_led_buffer, 0, num_leds * sizeof(uint32_t));
    }

    ~WS2812FX() {
        if(_led_buffer) free(_led_buffer);
    }

    void init() {
        // No-op for mock
    }

    void start() {
        _running = true;
        _mode_last_call_time = 0;
        trigger();
    }

    void stop() {
        _running = false;
    }

    void setCustomShow(show_callback p) {
        _custom_show = p;
    }

    void show() {
        if(_custom_show) {
            _custom_show();
        }
    }

    void service() {
        if(_running && (millis() - _mode_last_call_time > _mode_delay)) {
            if(_mode_ptr) {
                _mode_delay = (_mode_ptr)();
                _mode_last_call_time = millis();
                show();
            }
        }
    }

    void setMode(uint8_t m) {
        _mode_index = m;
        // Simplified: only support custom mode at FX_MODE_CUSTOM
        if (m == FX_MODE_CUSTOM) {
             _mode_ptr = _custom_mode;
             trigger(); // Only trigger if mode is valid
        } else {
            _mode_ptr = NULL;
        }
    }

    uint8_t getMode(void) {
        return _mode_index;
    }

    // Ignore index, always overwrite single slot
    void setCustomMode(uint8_t i, String name, mode_ptr p) {
        _custom_mode = p;
    }
    
    void setCustomMode(uint8_t i, const __FlashStringHelper* name, mode_ptr p) {
         _custom_mode = p;
    }

    void setSpeed(uint16_t s) {
        _speed = s;
    }

    uint16_t getSpeed(void) {
        return _speed;
    }

    void setBrightness(uint8_t b) {
        _brightness = b;
    }

    uint8_t getBrightness(void) {
        return _brightness;
    }

    void setColor(uint32_t c) {
        _color = c;
    }

    uint32_t getColor(void) {
        return _color;
    }

    void setPixelColor(uint16_t n, uint32_t c) {
        if(n < _num_leds) {
            _led_buffer[n] = c;
        }
    }

    void setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b) {
        setPixelColor(n, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }

    uint32_t getPixelColor(uint16_t n) {
        if(n < _num_leds) return _led_buffer[n];
        return 0;
    }

    void trigger() {
        _mode_delay = 0;
    }

    uint16_t getLength() { return _num_leds; }

private:
    uint16_t _num_leds;
    uint32_t* _led_buffer;
    bool _running;
    uint16_t _speed;
    uint8_t _brightness;
    uint32_t _color;
    
    uint8_t _mode_index;
    mode_ptr _mode_ptr;
    unsigned long _mode_last_call_time;
    uint16_t _mode_delay;
    show_callback _custom_show;
    mode_ptr _custom_mode; // Single mode
};

#endif
