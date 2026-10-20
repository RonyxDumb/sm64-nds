/* Reference wave formula before the distance cache. */
s16 calculate_ripple_at_point(struct Painting *painting, f32 posX, f32 posY) {

    f32 rippleMag = painting->currRippleMag;

    f32 rippleRate = painting->currRippleRate;

    f32 dispersionFactor = painting->dispersionFactor;

    f32 rippleTimer = painting->rippleTimer;

    f32 rippleX = painting->rippleX;
    f32 rippleY = painting->rippleY;

    f32 distanceToOrigin;
    f32 rippleDistance;

    posX *= painting->size / PAINTING_SIZE;
    posY *= painting->size / PAINTING_SIZE;
    distanceToOrigin = sqrtf((posX - rippleX) * (posX - rippleX) + (posY - rippleY) * (posY - rippleY));

    rippleDistance = distanceToOrigin / dispersionFactor;
    if (rippleTimer < rippleDistance) {

        return 0;
    } else {

        f32 rippleZ = rippleMag * cosf(rippleRate * (2 * M_PI) * (rippleTimer - rippleDistance));

        return round_float(rippleZ);
    }
}
