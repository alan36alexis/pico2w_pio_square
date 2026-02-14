/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
// Our assembled program:
#include "hello.pio.h"

// This example uses the default led pin
// You can change this by defining HELLO_PIO_LED_PIN to use a different gpio
#if !defined HELLO_PIO_LED_PIN
#define HELLO_PIO_LED_PIN 16
#endif

// Check the pin is compatible with the platform
#if HELLO_PIO_LED_PIN >= NUM_BANK0_GPIOS
#error Attempting to use a pin>=32 on a platform that does not support it
#endif

// Función para configurar el generador de onda cuadrada
void pio_square_wave_ms(PIO pio, uint sm, uint offset, uint period_ms, float duty_cycle) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t cycles_per_ms = sys_hz / 1000;
    uint32_t total_cycles = period_ms * cycles_per_ms;
    
    uint32_t high_cycles = (uint32_t)(total_cycles * duty_cycle);
    uint32_t low_cycles = total_cycles - high_cycles;

    // Reiniciar y configurar para modo infinito
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset + hello_offset_infinite));
    pio_sm_set_enabled(pio, sm, true);

    // Enviar los tiempos al FIFO (High, luego Low)
    pio_sm_put_blocking(pio, sm, high_cycles);
    pio_sm_put_blocking(pio, sm, low_cycles);
}

void pio_square_wave_us(PIO pio, uint sm, uint offset, uint period_us, float duty_cycle) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    // Usamos uint64_t para evitar desbordamiento en la multiplicación
    uint32_t total_cycles = (uint32_t)((uint64_t)period_us * sys_hz / 1000000);
    
    uint32_t high_cycles = (uint32_t)(total_cycles * duty_cycle);
    uint32_t low_cycles = total_cycles - high_cycles;

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset + hello_offset_infinite));
    pio_sm_set_enabled(pio, sm, true);

    pio_sm_put_blocking(pio, sm, high_cycles);
    pio_sm_put_blocking(pio, sm, low_cycles);
}

void pio_square_wave_ns(PIO pio, uint sm, uint offset, uint period_ns, float duty_cycle) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t total_cycles = (uint32_t)((uint64_t)period_ns * sys_hz / 1000000000);
    
    uint32_t high_cycles = (uint32_t)(total_cycles * duty_cycle);
    uint32_t low_cycles = total_cycles - high_cycles;

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset + hello_offset_infinite));
    pio_sm_set_enabled(pio, sm, true);

    pio_sm_put_blocking(pio, sm, high_cycles);
    pio_sm_put_blocking(pio, sm, low_cycles);
}

// Funciones para Burst (Ráfaga)
void pio_burst_ms(PIO pio, uint sm, uint offset, uint count, uint period_ms, float duty_cycle) {
    if (count == 0) return;
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t total_cycles = period_ms * (sys_hz / 1000);
    uint32_t high_cycles = (uint32_t)(total_cycles * duty_cycle);
    uint32_t low_cycles = total_cycles - high_cycles;

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset + hello_offset_burst));
    pio_sm_set_enabled(pio, sm, true);

    pio_sm_put_blocking(pio, sm, count - 1); // N-1 porque el bucle es do-while
    pio_sm_put_blocking(pio, sm, high_cycles);
    pio_sm_put_blocking(pio, sm, low_cycles);
}

void pio_burst_us(PIO pio, uint sm, uint offset, uint count, uint period_us, float duty_cycle) {
    if (count == 0) return;
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t total_cycles = (uint32_t)((uint64_t)period_us * sys_hz / 1000000);
    uint32_t high_cycles = (uint32_t)(total_cycles * duty_cycle);
    uint32_t low_cycles = total_cycles - high_cycles;

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset + hello_offset_burst));
    pio_sm_set_enabled(pio, sm, true);

    pio_sm_put_blocking(pio, sm, count - 1);
    pio_sm_put_blocking(pio, sm, high_cycles);
    pio_sm_put_blocking(pio, sm, low_cycles);
}

void pio_burst_ns(PIO pio, uint sm, uint offset, uint count, uint period_ns, float duty_cycle) {
    if (count == 0) return;
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t total_cycles = (uint32_t)((uint64_t)period_ns * sys_hz / 1000000000);
    uint32_t high_cycles = (uint32_t)(total_cycles * duty_cycle);
    uint32_t low_cycles = total_cycles - high_cycles;

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset + hello_offset_burst));
    pio_sm_set_enabled(pio, sm, true);

    pio_sm_put_blocking(pio, sm, count - 1);
    pio_sm_put_blocking(pio, sm, high_cycles);
    pio_sm_put_blocking(pio, sm, low_cycles);
}

int main() {
#ifndef HELLO_PIO_LED_PIN
#warning pio/hello_pio example requires a board with a regular LED
#else
    PIO pio;
    uint sm;
    uint offset;

    setup_default_uart();

    // This will find a free pio and state machine for our program and load it for us
    // We use pio_claim_free_sm_and_add_program_for_gpio_range so we can address gpios >= 32 if needed and supported by the hardware
    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(&hello_program, &pio, &sm, &offset, HELLO_PIO_LED_PIN, 1, true);
    hard_assert(success);

    // Configure it to run our program, and start it, using the
    // helper function we included in our .pio file.
    printf("Using gpio %d\n", HELLO_PIO_LED_PIN);
    hello_program_init(pio, sm, offset, HELLO_PIO_LED_PIN);

    // The state machine is now running. Any value we push to its TX FIFO will
    // determine the wave parameters.
    printf("Iniciando generador de onda cuadrada en GP%d\n", HELLO_PIO_LED_PIN);
    
    // Configurar: Periodo 1000ms (1Hz), Ciclo de actividad 50%
    // pio_square_wave_ns(pio, sm, offset, 100, 0.5f);
    pio_burst_ms(pio, sm, offset, 5, 500, 0.25f); // 5 pulsos, periodo 500ms, ciclo de actividad 25%

    // El PIO se encarga de todo ahora, el CPU puede dormir o hacer otras cosas
    while (true) {
        tight_loop_contents();
    }

    // This will free resources and unload our program
    pio_remove_program_and_unclaim_sm(&hello_program, pio, sm, offset);
#endif
}
