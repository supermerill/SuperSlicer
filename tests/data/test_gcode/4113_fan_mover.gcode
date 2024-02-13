
;WIDTH:0.237984
M106 S35.7 ; set fan for Gap fill
G1 X40.391 Y46.717 E0.00302 ; Gap fill
G1 X40.369 Y46.899 E0.00245 ; Gap fill
M106 S20.4 ; set default fan
M106 S51 ; set fan for new extruder
;LAYER_CHANGE
G1 Z0.68 F3000 ; move to next layer (2)
SET_PRINT_STATS_INFO CURRENT_LAYER=3
G92 E0 ; reset extruder position to flush any extruder axis rounding
M204 S12000 ; adjust acceleration
G1 X51.732 Y55.09 F42000 ; move to first perimeter point (acceleration)
M204 S7000 ; adjust acceleration
G1 X59.535 Y60.716 ; move to first perimeter point (deceleration)
SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=5
;WIDTH:0.45
M106 S51 ; set fan for Internal perimeter
G1 F15681
G1 X31.554 Y60.716 E0.78256 ; perimeter
G1 X31.554 Y32.734 E0.78256 ; perimeter
G1 X59.535 Y32.734 E0.78256 ; perimeter
G1 X59.535 Y60.656 E0.78088 ; perimeter
M106 S51 ; set default fan
G1 X59.942 Y61.123 F42000 ; move to first perimeter point (minimum acceleration)
M106 S51 ; set fan for Internal perimeter
G1 F15681
G1 X31.147 Y61.123 E0.80533 ; perimeter
G1 X31.147 Y32.327 E0.80533 ; perimeter
G1 X59.942 Y32.327 E0.80533 ; perimeter
G1 X59.942 Y61.063 E0.80365 ; perimeter
M106 S51 ; set default fan
M204 S6000 ; adjust acceleration
G1 X60.335 Y61.515 F42000
M106 S51 ; set fan for External perimeter
G1 F15000
G1 X30.755 Y61.515 E0.76629 ; perimeter
G1 X30.755 Y31.935 E0.76629 ; perimeter
G1 X60.335 Y31.935 E0.76629 ; perimeter
G1 X60.335 Y61.455 E0.76474 ; perimeter
M106 S51 ; set default fan
;WIPE_START
M204 S7000 ; adjust acceleration
G1 X59.988 Y61.315 F42000 ; move inwards before travel
;WIPE_END
G1 X58.97 Y60.501 ; move to first Internal infill point (minimum acceleration)
; custom gcode: feature_gcode
M106 S51 ; set fan for Internal infill
G1 F15681
G1 X56.527 Y60.501 E0.06831 ; Internal infill
G1 X59.321 Y50.076 E0.30183 ; Internal infill
