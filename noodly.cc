#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <vector>

// LED strip Libraries
#include <spixels/led-strip.h>
#include <spixels/multi-spi.h>

// Touch library
#include <MPR121.h>

// Microorb
#include "microorb.h"

using namespace spixels;
using namespace orb_driver;

#define TOUCH_MPR121_ADDRESS 0x5A      // I2C address of touch-sensor.
#define LED_STRIP_CLOCK_SPEED_MHZ 1    // Safe bet for LPD8806
#define SOUND_BINARY "/usr/bin/aplay"  // Binary to run

// After we have set up GPIO, we drop privileges to this user, as we execute
// the aplay binary later. User 1000 is just the default pi user.
#define PI_USER 1000

#define NOODLY_APPENDAGES 12           // Number of touch-sensors and LED strips
#define NOODLY_LEDS       240          // LEDs pre LED strips.
#define NOODLY_DEFAULT_COLOR 0xffff00  // Noodly yellow default animation color
#define NOODLY_ANIMATION_SLOWDOWN 2
#define NOODLY_RETRIGGER false         // 'true' to allow retrigger
#define NOODLY_PIXEL_REPEAT 2          // repeating pixels on strip.

// The sequence of colors we play starting from the outside in.
static uint32_t kAnimationColors[] = {
    0xA000FF,  // violet
    0x0000FF,  // blue
    0x00FF00,  // green
    0xFFFF00,  // yellow
    0xFF9000,  // orange
    0xFF0000,  // red
};

// Essentially, when we reach the eye, we just play the same sequence,
// followed by a long time of white...
static orb_sequence_t kEyeOrbSequence = {
    8,   // Number of elements below.
    {
        // { R,G,B Color} , morph-time, hold-time (time units: 250ms)
        { {0xff, 0x00, 0x00 }, 2, 1 },
        { {0xff, 0xff, 0x00 }, 2, 1 },
        { {0x00, 0xff, 0x00 }, 2, 1 },
        { {0x00, 0x00, 0xff }, 2, 1 },
        { {0xa0, 0x00, 0xff }, 2, 1 },
        // Last color is white, and we gradually morph into it from the
        // violet. We leave it on the longst possible time (about 60 seconds)
        // then enqueue a couple of more white so that it essentially stays
        // white if nobody touches anything.
        { {0xff, 0xff, 0xff }, 10, 255 },
        { {0xff, 0xff, 0xff }, 0, 255 },
        { {0xff, 0xff, 0xff }, 0, 255 },
    }
};

// Multiplexed animation: Every LEDStripAnimation handles its own animation.
// It gets regular timeslice call to UpdateAnimationFrame() in which it can
// update its state.
class LEDStripAnimation {
public:
    LEDStripAnimation(LEDStrip *strip)
        : strip_(strip), random_per_strip_(random()),
          animation_pos_(-1), animation_clock_(0) {}

    // Trigger a new animation.
    void ButtonTriggered(bool is_on) {
        if (!is_on) return;
        // We let the animation run to the end first unless NOODLY_RETRIGGER
        if (NOODLY_RETRIGGER || animation_pos_ < 0) {
            animation_pos_ = strip_->count();
        }
    }

    // Update the output. Called once per time-slice.
    // Returns true when last animation phase is done.
    bool UpdateAnimationFrame() {
        // We only update on every other
        if ((animation_clock_++ % NOODLY_ANIMATION_SLOWDOWN) != 0)
            return false;

        // Regular background effect. Some sinusoidal wave.
        // We don't want all LED strips be in
        // phase, so we have some randomness per strip.
        const uint32_t background_phase
            = (random_per_strip_ + animation_clock_/2) % strip_->count();
        for (int i = 0; i < strip_->count(); ++i) {
            float fraction = (3.0 * i + background_phase) / strip_->count();
            float position_bright = cosf(2 * M_PI * fraction);
            uint8_t col = (position_bright + 1) * 63 + 64;
            strip_->SetPixel(i, (col << 16) | (col << 8) | 0);
        }

        // Current active animation, walking up the strip.
        if (animation_pos_ < 0)
            return false;

        // Rainbow
        int col_pos = animation_pos_-1;
        for (uint32_t color : kAnimationColors) {
            for (int i = 0; i < NOODLY_PIXEL_REPEAT; ++i)
                strip_->SetPixel(col_pos--, color);
        }

        animation_pos_--;
        return animation_pos_ == -1;
    }

private:
    LEDStrip *const strip_;
    const uint32_t random_per_strip_;

    // Wherever the rainbow is currently.
    // Positive number if active, -1 if idle. Right now only one.
    int animation_pos_;

    uint32_t animation_clock_;
};

static LEDStripAnimation *CreateAnim(MultiSPI *spi, int connector, int leds) {
    return new LEDStripAnimation(CreateLPD8806Strip(spi, connector, leds));
}

static std::vector<MicroOrb*> GetAvailableEyes() {
    std::vector<MicroOrb*> result;
    MicroOrb::DeviceList devices;
    MicroOrb::UsbList(&devices);
    for (auto d : devices) {
        auto orb = MicroOrb::Open(d);
        if (orb) result.push_back(orb);
    }
    return result;
}

static void PlaySound(const char *file) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s %s &", SOUND_BINARY, file);
    system(buffer);
}

int main(int argc, char *argv[]) {
    std::vector<const char *> files;
    for (int i = 1; i < argc; ++i) {
        fprintf(stderr, "Adding file %s\n", argv[i]);
        files.push_back(argv[i]);
    }

    auto eyes = GetAvailableEyes();
    for (auto e : eyes) {
        e->SetColor({0xff, 0xff, 0xff});
    }

    MPR121.begin(TOUCH_MPR121_ADDRESS);

    MultiSPI *const spi = CreateDirectMultiSPI(LED_STRIP_CLOCK_SPEED_MHZ);
    LEDStripAnimation *animation[NOODLY_APPENDAGES] = {
        // NOTE: the first LED strip needs to be the one with the most amount
        // of LEDs as there is some issue with calling new after a ralloc()
        // on the Pi.¯\_(ツ)_/¯
        CreateAnim(spi, spixels::MultiSPI::SPI_P1, NOODLY_LEDS),

        CreateAnim(spi, spixels::MultiSPI::SPI_P2, 48 /*NOODLY_LEDS*/),
        CreateAnim(spi, spixels::MultiSPI::SPI_P3, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P4, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P5, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P6, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P7, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P8, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P9, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P10, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P11, NOODLY_LEDS),
        CreateAnim(spi, spixels::MultiSPI::SPI_P12, NOODLY_LEDS),
    };

    // Drop privs
    setresuid(PI_USER, PI_USER, PI_USER);
    setresgid(PI_USER, PI_USER, PI_USER);

    for (;;) {
        usleep(10000);

        MPR121.updateTouchData();
        for (int i = 0; i < NOODLY_APPENDAGES; ++i) {
            animation[i]->ButtonTriggered(MPR121.getTouchData(i));
        }

        bool any_animation_reached_end = false;
        for (int i = 0; i < NOODLY_APPENDAGES; ++i) {
            any_animation_reached_end |= animation[i]->UpdateAnimationFrame();
        }

        spi->SendBuffers(); // All animations updated: send at once.

        if (any_animation_reached_end) {
            PlaySound(files[random() % files.size()]);
            for (auto e : eyes) e->SetSequence(kEyeOrbSequence);
        }
    }
    return 0;
}
