/**
 * Capture the real PSG output: load a ROM, run the core, and look at what
 * the APU actually produces. This is the test dectest cannot be - dectest
 * feeds synthetic signals through the decimator, but never hears the PSG.
 *
 * The question it answers: does the pipeline from Gambatte's PSG to the
 * decimator's output produce something that looks like game audio, or does
 * it produce silence, noise, or a DC offset?
 *
 *   make -C tools/host psgtest ROM=/path/to/rom.gb
 *
 * NOT part of the 3DS build.
 */
#include <3ds.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "gambatte.h"
#include "decimator.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

#define RUN_SAMPLES 2064
#define SOUND_BUF   (RUN_SAMPLES + 2064)
#define FRAMES      120           /* ~2 seconds                          */

int main(int argc, char **argv)
{
    const char *rom_path = (argc > 1) ? argv[1] : "../../test/dmg-acid2.gb";

    gambatte::GB core;
    core.setColorCorrection(false);

    FILE *f = fopen(rom_path, "rb");
    if (f == NULL)
    {
        printf("psgtest: cannot open %s\n", rom_path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = (uint8_t *)malloc(size);
    fread(rom, 1, size, f);
    fclose(f);

    if (core.load(rom, (unsigned)size, gambatte::GB::FORCE_DMG) != 0)
    {
        printf("psgtest: core refused the ROM\n");
        free(rom);
        return 2;
    }

    gambatte::uint_least32_t sound_buf[SOUND_BUF];
    gambatte::video_pixel_t vbuf[144 * 160];
    decimator stage;
    decimator_reset(&stage);

    /* Capture buffers (heap: 120 frames * 35112 = 4.2M words = 16 MB) */
    const int max_psg = FRAMES * 35112 / 64 + 2;
    int16_t *dec_l = (int16_t *)malloc(max_psg * sizeof(int16_t));
    int16_t *dec_r = (int16_t *)malloc(max_psg * sizeof(int16_t));
    int total_psg = 0, total_dec = 0;

    /* Also capture raw PSG words for analysis */
    const int max_raw = FRAMES * 35112;
    uint32_t *raw = (uint32_t *)malloc(max_raw * sizeof(uint32_t));
    int total_raw = 0;

    int nonzero_samples = 0;
    int64_t sum_l = 0, sum_r = 0;
    int max_abs_l = 0, max_abs_r = 0;

    for (int frame = 0; frame < FRAMES; frame++)
    {
        for (;;)
        {
            unsigned samples = RUN_SAMPLES;
            long ret = core.runFor(vbuf, 160, sound_buf, SOUND_BUF, samples);

            if (samples > SOUND_BUF)
                samples = SOUND_BUF;

            /* Capture raw PSG words */
            for (unsigned i = 0; i < samples && total_raw < max_raw; i++)
            {
                raw[total_raw++] = sound_buf[i];
                if (sound_buf[i] != 0x80008000UL)
                    nonzero_samples++;
            }

            /* Analyze raw PSG output */
            for (unsigned i = 0; i < samples; i++)
            {
                int32_t l = (int16_t)(sound_buf[i] & 0xFFFFu);
                int32_t r = (int16_t)(sound_buf[i] >> 16);
                sum_l += l;
                sum_r += r;
                int a = l > 0 ? l : -l;
                if (a > max_abs_l) max_abs_l = a;
                a = r > 0 ? r : -r;
                if (a > max_abs_r) max_abs_r = a;
            }

            /* Run through decimator */
            int16_t audio_out[72 * 2];
            unsigned out = decimator_run(&stage, sound_buf, samples, audio_out);

            for (unsigned i = 0; i < out && total_dec < max_psg; i++)
            {
                dec_l[total_dec] = audio_out[i * 2];
                dec_r[total_dec] = audio_out[i * 2 + 1];
                total_dec++;
            }

            total_psg += samples;

            if (ret >= 0)
                break;
        }
    }

    double mean_l = (double)sum_l / total_psg;
    double mean_r = (double)sum_r / total_psg;
    double rms_l = 0, rms_r = 0;

    for (int i = 0; i < total_dec; i++)
    {
        rms_l += (double)dec_l[i] * dec_l[i];
        rms_r += (double)dec_r[i] * dec_r[i];
    }
    rms_l = sqrt(rms_l / total_dec);
    rms_r = sqrt(rms_r / total_dec);

    printf("psgtest: %d frames, %d raw samples, %d decimated samples\n",
           FRAMES, total_raw, total_dec);

    /* PSG raw analysis */
    printf("psgtest: raw PSG -- nonzero %d/%d (%.1f%%), "
           "mean L=%.1f R=%.1f, max |L|=%d max |R|=%d\n",
           nonzero_samples, total_raw,
           100.0 * nonzero_samples / total_raw,
           mean_l, mean_r, max_abs_l, max_abs_r);

    /* Decimated analysis */
    printf("psgtest: decimated -- RMS L=%.1f R=%.1f, "
           "mean L=%.1f R=%.1f\n",
           rms_l, rms_r,
           (double)sum_l / total_psg, (double)sum_r / total_psg);

    /* Checks */
    check("PSG produces non-silent output", nonzero_samples > total_raw * 0.01);
    check("PSG left channel has signal", max_abs_l > 100);
    check("PSG right channel has signal", max_abs_r > 100);
    check("Decimated left has signal", rms_l > 10.0);
    check("Decimated right has signal", rms_r > 10.0);
    check("No huge DC offset (left)", fabs(mean_l) < 10000.0);
    check("No huge DC offset (right)", fabs(mean_r) < 10000.0);

    /* Write raw PSG output to a file for offline analysis */
    FILE *rf = fopen("/tmp/psg-raw.bin", "wb");
    if (rf != NULL)
    {
        fwrite(raw, 4, total_raw, rf);
        fclose(rf);
        printf("psgtest: wrote %d raw PSG words to /tmp/psg-raw.bin\n", total_raw);
    }

    /* Write decimated output as a WAV */
    FILE *wf = fopen("/tmp/psg-dec.wav", "wb");
    if (wf != NULL)
    {
        uint32_t riff[4] = {0x52494646, 0, 0x45564157}; /* RIFF....WAVE */
        uint32_t fmt[4] = {0x666d7420, 16, 1, 32768};  /* fmt  16 1 32768 */
        uint16_t channels = 2, bits = 16;
        uint32_t data_size = total_dec * 4;
        uint32_t riff_size = 36 + data_size;

        fwrite(riff, 4, 1, wf);
        fwrite(&riff_size, 4, 1, wf);
        fwrite(&fmt[2], 4, 1, wf);  /* skip "fmt " */
        fwrite(&channels, 2, 1, wf);
        fwrite(&bits, 2, 1, wf);
        fwrite(&data_size, 4, 1, wf);

        for (int i = 0; i < total_dec; i++)
        {
            int16_t interleaved[2] = {dec_l[i], dec_r[i]};
            fwrite(interleaved, 2, 2, wf);
        }
        fclose(wf);
        printf("psgtest: wrote %d decimated frames to /tmp/psg-dec.wav\n", total_dec);
    }

    free(rom);
    free(dec_l);
    free(dec_r);
    free(raw);

    printf("psgtest: %s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
