
#ifndef MODEL_HANDLER_H__
#define MODEL_HANDLER_H__

#include <zephyr/bluetooth/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inicjuje modele mesh wezla i zwraca kompozycje */
const struct bt_mesh_comp *model_handler_init(void);

/* Publikuje sztuczna wartosc temperatury przez Sensor Server */
int model_handler_publish_temp(void);

/* Czy Friend skonczyl konfiguracje - decyduje, kiedy mozna przejsc w tryb LPN */
bool model_handler_is_configured(void);

/* Odtwarza konfiguracje lokalnie, zamiast czekac na Config Client Frienda */
int model_handler_restore_config(uint16_t net_idx, uint16_t app_idx, const uint8_t app_key[16],
                                 uint16_t pub_addr);

#ifdef __cplusplus
}
#endif

#endif /* MODEL_HANDLER_H__ */
