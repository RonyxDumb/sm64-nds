/* Exact original double-precision packing reference. */
s8 normalize_component(f32 comp) {
    s8 rounded;

    if (comp > 0.0) {
        rounded = comp * 127.0 + 0.5;
    } else if (comp < 0.0) {
        rounded = comp * 128.0 - 0.5;
    } else {
        rounded = 0;
    }
    return rounded;
}
