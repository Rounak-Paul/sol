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
        .width  = 800,
        .height = 600,
    });
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        ca_instance_destroy(instance);
        return 1;
    }

    ca_ui_begin(window, &(Ca_DivDesc){
        .direction  = CA_VERTICAL,
        .background = ca_color(0.05f, 0.05f, 0.07f, 1.0f),
        .padding    = { 24.0f, 24.0f, 24.0f, 24.0f },
    });

    ca_h1(&(Ca_TextDesc){
        .text  = "Hello Sol",
        .color = ca_color(0.9f, 0.9f, 0.9f, 1.0f),
    });

    ca_ui_end();

    while (ca_instance_tick(instance)) {}

    ca_instance_destroy(instance);
    return 0;
}
