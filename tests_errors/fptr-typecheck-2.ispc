// Can't convert argument of type "void * const uniform" to type "float" for funcion call argument.

float bar(float a, float b);

export uniform int foo(uniform int x[], uniform int i[]) {
    float (*fptr)(float, float) = bar;
//CO    bar(0,1,2);
    fptr(NULL, 1);
}
