#include "settings.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
const char *platform_data_dir(void) { return "."; }
int main(void) {
    const float values[]={0.01f,0.0125f,0.05f,0.1f,0.5f,6.0f,30.0f};
    for(unsigned i=0;i<sizeof values/sizeof values[0];i++) {
        Settings a,b;settings_defaults(&a);settings_defaults(&b);a.mouse_sens=values[i];
        assert(settings_save(&a,"mouse-sensitivity-test.ini"));
        assert(settings_load(&b,"mouse-sensitivity-test.ini"));
        assert(fabsf(a.mouse_sens-b.mouse_sens)<0.000001f);
    }
    assert(settings_clamp_mouse_sensitivity(NAN)==MOUSE_SENS_DEFAULT);
    assert(settings_clamp_mouse_sensitivity(INFINITY)==MOUSE_SENS_DEFAULT);
    assert(settings_clamp_mouse_sensitivity(-1)==MOUSE_SENS_MIN);
    assert(settings_clamp_mouse_sensitivity(31)==MOUSE_SENS_MAX);
    remove("mouse-sensitivity-test.ini");return 0;
}
