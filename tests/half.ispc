
export uniform int width() { return programCount; }

export void f_v(uniform float RET[]) {
    float sum = 0;
    int errors = 0;
    for (uniform int i = 0; i <= 0xffff; ++i) {
        unsigned int16 h = i;
        float f = half_to_float(i);
        h = float_to_half(f);

        // may return a different value back for NaNs..
        if (f == f && i != h)
            ++errors;
    }
    RET[programIndex] = errors;
}

export void result(uniform float RET[]) {
    RET[programIndex] = 0;
}
