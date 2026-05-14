const examples = [
    {
        'description': '16:10 screen side to side with a 3:2 screen',
        'config':
        {
            "version": 7,
            "unmapped_passthrough": true,
            "partial_scroll_timeout": 1000000,
            "interval_override": 0,
            "constraint_mode": 2,
            "offscreen_sensitivity": 4000,
            "cursor_placement_interval_seconds": 0,
            "mouse": {
                "tracking_speed": 0.6875,
                "pointer_resolution": 400.0,
                "frame_rate": 67.0,
                "fixed_multiplier": 1.0,
                "placement_tolerance": 0.5
            },
            "screens": [
                {
                    "x": 0,
                    "y": 0,
                    "w": 1440000,
                    "h": 900000,
                    "sensitivity": 8000
                },
                {
                    "x": 1440000,
                    "y": 0,
                    "w": 1350000,
                    "h": 900000,
                    "sensitivity": 8000
                }
            ],
            "mappings": [
            ]
        }
    },
    {
        'description': 'two 16:9 screens, one on top of the other',
        'config':
        {
            "version": 7,
            "unmapped_passthrough": true,
            "partial_scroll_timeout": 1000000,
            "interval_override": 0,
            "constraint_mode": 2,
            "offscreen_sensitivity": 4000,
            "cursor_placement_interval_seconds": 0,
            "mouse": {
                "tracking_speed": 0.6875,
                "pointer_resolution": 400.0,
                "frame_rate": 67.0,
                "fixed_multiplier": 1.0,
                "placement_tolerance": 0.5
            },
            "screens": [
                {
                    "x": 0,
                    "y": 0,
                    "w": 1920000,
                    "h": 1080000,
                    "sensitivity": 4000
                },
                {
                    "x": 0,
                    "y": 1080000,
                    "w": 1920000,
                    "h": 1080000,
                    "sensitivity": 4000
                }
            ],
            "mappings": [
            ]
        }
    },
];

export default examples;
