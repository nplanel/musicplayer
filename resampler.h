#pragma once

#include <array>
#include <coreutils/fifo.h>

struct resampler;

resampler* rs_create();
void rs_delete(resampler*);
resampler* rs_dup(const resampler*);
void rs_dup_inplace(resampler*, const resampler*);

int rs_get_free_count(resampler*);
void rs_write_sample(resampler*, short sample_l, short sample_r);
void rs_set_rate(resampler*, double new_factor);
int rs_ready(resampler*);
void rs_clear(resampler*);
int rs_get_sample_count(resampler*);
void rs_get_sample(resampler*, short* sample_l, short* sample_r);
void rs_remove_sample(resampler*);

template <int SIZE> struct Resampler
{
    int targetHz;
    bool active = false;
    resampler* samp;
    utils::Ring<int16_t, SIZE> fifo;
    explicit Resampler(int hz = 44100) : targetHz(hz), samp(rs_create())
    {
        setHz(hz);
    }

    void setHz(int hz)
    {
        active = hz != targetHz;
        rs_set_rate(samp, static_cast<float>(hz) / targetHz);
    }

    void write(int16_t* left, int16_t* right, size_t size, int stride = 2)
    {
        if (!active && stride == 2) {
            fifo.write(left, size);
            return;
        }
        for (int i = 0; i < size; i += stride) {
            rs_write_sample(samp, left[i], right[i]);
            while (rs_get_sample_count(samp) > 0) {
                std::array<int16_t, 2> lr;
                rs_get_sample(samp, &lr[0], &lr[1]);
                fifo.write(&lr[0], 2);
                rs_remove_sample(samp);
            }
        }
    }

    size_t read(int16_t* target, size_t n) { return fifo.read(target, n); }
};
