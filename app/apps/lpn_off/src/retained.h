
#ifndef RETAINED_H__
#define RETAINED_H__

#include <stdbool.h>
#include <stdint.h>

/* Stan mesh przenoszony przez System OFF w podtrzymanej sekcji RAM */
struct retained_data {
    uint32_t magic;      /* znacznik formatu - odsiewa smieci po zimnym starcie */
    uint32_t cycles;     /* licznik cykli pomiarowych od zimnego startu */
    uint32_t iv_index;   /* IV Index sieci */
    uint32_t seq;        /* nastepny seq do wyslania - musi rosnac monotonicznie */
    uint16_t addr;       /* adres unicast przydzielony przez Frienda */
    uint8_t provisioned; /* 1 = mamy komplet danych do odtworzenia sieci */
    uint8_t reserved;    /* wyrownanie */
    uint32_t crc;        /* CRC32 wszystkiego przed soba - musi byc ostatnie */
};

extern struct retained_data retained;

/* Wczytuje blok retencyjny; false = brak sprzetowej retencji albo zle CRC */
bool retained_load(void);

/* Zapisuje blok retencyjny wraz z nowym CRC */
void retained_save(void);

#endif /* RETAINED_H__ */
