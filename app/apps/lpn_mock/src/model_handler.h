
#ifndef MODEL_HANDLER_H__
#define MODEL_HANDLER_H__

#include <zephyr/bluetooth/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inicjuje modele mesh wezla i zwraca kompozycje (dla bt_mesh_init). */
const struct bt_mesh_comp *model_handler_init(void);

/* Odczytuje temperature (mock: stala wartosc) i publikuje ja przez Sensor
 * Server. Zwraca 0 lub kod bledu. */
int model_handler_publish_temp(void);

/* Czy konfiguracja modeli jest kompletna - tzn. Friend ustawil juz adres
 * publikacji Sensor Servera (mod_pub_set to OSTATNI krok konfiguracji). Sluzy
 * do decyzji, kiedy wezel moze bezpiecznie przejsc w tryb LPN. */
bool model_handler_is_configured(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEL_HANDLER_H__ */
