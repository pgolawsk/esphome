from . import EpaperModel

# WeAct 2.9" 3-color (Black/White/Red) display
# Controller: SSD1680
# Resolution: 128x296

weact_2p9in_3c = EpaperModel(
    "weact-2.9in-3c",
    class_name="EPaperWeAct3C",
    width=128,
    height=296,
    data_rate="10MHz",
    minimum_update_interval="27s",
    initsequence=(
        # Software reset
        (0x12,),
        # Driver output control - Y start address: 0x0127 (295), Y end: 0x0000
        (0x01, 0x27, 0x01, 0x00),
        # Data entry mode - Y increment, X increment (0x03)
        (0x11, 0x03),
        # Border waveform
        (0x3C, 0x05),
        # Read built-in temperature sensor
        (0x18, 0x80),
        # Display update control
        (0x21, 0x00, 0x80),
        # RAM X address range: 0 to 15 (128/8 - 1)
        (0x44, 0x00, 0x0F),
        # RAM Y address range: 0 to 295 (0x0127)
        (0x45, 0x00, 0x00, 0x27, 0x01),
        # RAM X address counter
        (0x4E, 0x00),
        # RAM Y address counter
        (0x4F, 0x00, 0x00),
    ),
)

# WeAct 4.2" 3-color (Black/White/Red) display
# Controller: SSD1680
# Resolution: 400x300

weact_4p2in_3c = EpaperModel(
    "weact-4.2in-3c",
    class_name="EPaperWeAct3C",
    width=400,
    height=300,
    data_rate="10MHz",
    minimum_update_interval="27s",
    initsequence=(
        # Software reset
        (0x12,),
        # Driver output control - Y start address: 0x012B (299), Y end: 0x0000
        (0x01, 0x2B, 0x01, 0x00),
        # Data entry mode - Y increment, X increment (0x03)
        (0x11, 0x03),
        # Border waveform
        (0x3C, 0x05),
        # Read built-in temperature sensor
        (0x18, 0x80),
        # Display update control
        (0x21, 0x00, 0x80),
        # RAM X address range: 0 to 49 (400/8 - 1)
        (0x44, 0x00, 0x31),
        # RAM Y address range: 0 to 299 (0x012B)
        (0x45, 0x00, 0x00, 0x2B, 0x01),
        # RAM X address counter
        (0x4E, 0x00),
        # RAM Y address counter
        (0x4F, 0x00, 0x00),
    ),
)
