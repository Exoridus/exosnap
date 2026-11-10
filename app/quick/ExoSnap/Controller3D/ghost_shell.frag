// Ghost keeps the shell readable through its silhouette: faces seen edge-on gain
// opacity and rim light, faces seen head-on stay nearly clear.
void MAIN()
{
    float facing = abs(dot(normalize(NORMAL), normalize(VIEW_VECTOR)));
    float rim = pow(1.0 - facing, 3.0);
    BASE_COLOR = vec4(fillColor.rgb, clamp(0.05 + 0.6 * rim, 0.0, 1.0));
    EMISSIVE_COLOR = rimColor.rgb * rim * 0.5;
    ROUGHNESS = 0.9;
    METALNESS = 0.0;
}
