#include <causality.h>
#include <stdio.h>

int main(void)
{
    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Sol",
        .prefer_dedicated_gpu = true,
        .font_size_px         = 14.0f,
    });
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        return 1;
    }

    Ca_Window *window = ca_window_create(instance, &(Ca_WindowDesc){
        .title  = "Sol",
        .width  = 600,
        .height = 400,
    });
    if (!window) {
        ca_instance_destroy(instance);
        return 1;
    }

    ca_ui_begin(window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
    });

    ca_text(&(Ca_TextDesc){ .text = "Hello from Sol" });

    ca_ui_end();

    while (ca_instance_tick(instance)) {  }

    ca_instance_destroy(instance);
    return 0;
}
