
#ifndef MODEL_HANDLER_H__
#define MODEL_HANDLER_H__

#include <zephyr/bluetooth/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inicjuje modele mesh wezla i zwraca kompozycje */
const struct bt_mesh_comp *model_handler_init(void);

/* Odczytuje temperature z STS4X i publikuje ja przez Sensor Server */
int model_handler_publish_temp(void);

/* Czy Friend skonczyl konfiguracje - decyduje, kiedy mozna przejsc w tryb LPN */
bool model_handler_is_configured(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEL_HANDLER_H__ */
