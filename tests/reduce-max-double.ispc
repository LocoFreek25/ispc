
export uniform int width() { return programCount; }



export void f_fu(uniform float RET[], uniform float aFOO[], uniform float b) {
    double v = aFOO[programIndex];
    uniform float m;
    if (v >= 3)
        m = reduce_max(-v);
    RET[programIndex] = m;
}

export void result(uniform float RET[]) { RET[programIndex] = -3; }
