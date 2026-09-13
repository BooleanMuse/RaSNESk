// ===========================================================================
// Where everything sits on the four panels, in millimetres.
//
// The panels are drawn from these numbers rather than from an SVG. A console
// is a machine with a screen on it, and a plugin whose artwork lives in a
// file would have the labels in one place and the components in another,
// drifting apart at every change. Here there is one set of coordinates and
// both the paint and the components come off it.
// ===========================================================================
#pragma once

namespace racksnes {
namespace layout {

// Rack's grid, in Rack's units: one HP is 15 px and a 3U panel is 380 px, and
// RackWidget::addModule compares a module's height against that number
// exactly and throws if it misses. 380 px is 128.693 mm, not the 128.5 mm of
// a physical Eurorack panel -- use the physical figure and the module builds,
// loads, draws in the browser, and then aborts Rack the moment it is dropped
// into the rack.
constexpr float PxPerMm = 75.f / 25.4f;
constexpr float GridW   = 15.f;                  // RACK_GRID_WIDTH
constexpr float GridH   = 380.f;                 // RACK_GRID_HEIGHT

constexpr float HP      = GridW / PxPerMm;       // 5.08 mm
constexpr float PanelH  = GridH / PxPerMm;       // 128.693 mm

// Nothing starts before this or ends after the panel width less this: a
// corner screw is 5.08 mm square and sits in both corners.
constexpr float Margin  = 5.5f;
constexpr float TitleY  = 7.8f;

// The lid of the machine, across the top of every panel: the mascot, the
// module's name, and the controller's face. The same on all four, so that
// four of them in a row read as one set of hardware.
constexpr float DeckY      = 2.0f;
constexpr float DeckH      = 11.5f;
constexpr float MascotY    = 2.6f;
constexpr float MascotSize = 10.4f;
constexpr float NameY      = 11.6f;

// ---------------------------------------------------------------------------
// SNES -- the console
// ---------------------------------------------------------------------------
namespace snes {

constexpr int   Hp     = 60;
constexpr float PanelW = Hp * HP;                // 304.8

// The picture, at three panel pixels to every two of the console's: 384 x 336
// for its 256 x 224. That is 130 x 113.8 mm, and a 3U panel is 128.7 mm tall,
// so this is as large as a Super Nintendo gets on one -- twice size would be
// 152 mm down and there is nowhere to put it.
constexpr float ScreenScale = 1.5f;
constexpr float ScreenX = 13.0f;                 // clear of the screw column
constexpr float ScreenY = 4.0f;
constexpr float ScreenW = 256.f * ScreenScale / PxPerMm;    // 130.03
constexpr float ScreenH = 224.f * ScreenScale / PxPerMm;    // 113.78
constexpr float Bezel   = 2.0f;

// The toolbar under the picture: six buttons with pictures on them, and the
// cartridge's name in a window beside them, which is the shape a Mario Paint
// screen has.
constexpr float BarY      = ScreenY + ScreenH + 2.0f;        // 119.78
constexpr float BarH      = PanelH - BarY - 0.6f;            // 8.3
constexpr int   ButtonCount = 6;
constexpr float ButtonD   = 7.6f;
constexpr float ButtonY   = BarY + BarH / 2;
inline constexpr float buttonX(int i) { return ScreenX + 4.4f + 9.4f * i; }

constexpr float PlateX = ScreenX + 58.0f;
constexpr float PlateW = ScreenX + ScreenW - PlateX;

// The console body, to the right of the television.
constexpr float ColX    = 148.0f;
constexpr float ColR    = 296.0f;
constexpr float ColW    = ColR - ColX;           // 148

// Eight rows, one to a voice of the sound chip: what it is playing, how loud,
// and at what pitch. The one thing a panel can show that a jack cannot.
constexpr float MeterY     = 16.5f;
constexpr float MeterPitch = 3.7f;
constexpr float MeterH     = MeterPitch * 8;     // 29.6
inline constexpr float meterY(int i) { return MeterY + MeterPitch * i; }

// RATE and VOLUME stand beside the voice meters rather than under them:
// everything below is the controller, and the controller wants the room.
constexpr float MeterW  = 114.0f;
constexpr float RateX   = ColX + 124.0f;
constexpr float RateY   = 24.0f;
constexpr float VolX    = RateX;
constexpr float VolY    = 42.0f;
// The name goes beside the knob rather than under it: under is where the
// other knob is, and two names and two knobs in the same twenty millimetres
// read as a column of four things rather than as two.
constexpr float KnobNameX = RateX + 7.6f;

// ---------------------------------------------------------------------------
// The gamepad, laid out as a gamepad.
//
// Twelve sockets in the shape the hand already knows: a cross, a diamond on
// its grey disc, two pills in the middle and two shoulders on top. A row of
// twelve sockets labelled UP DOWN LEFT RIGHT would carry the same signals and
// tell you nothing; this is a Super Famicom controller and reads as one from
// across a room.
// ---------------------------------------------------------------------------
constexpr float PadCrossX  = ColX + 30.0f;
constexpr float PadCrossY  = 78.0f;
constexpr float PadSpace   = 10.5f;

constexpr float FaceX      = ColX + 94.0f;
constexpr float FaceY      = 78.0f;
constexpr float FaceSpace  = 10.8f;
constexpr float FaceDisc   = 15.0f;              // the grey disc's radius

constexpr float ShoulderY  = 55.0f;
constexpr float ShoulderLX = ColX + 20.0f;
constexpr float ShoulderRX = ColX + 104.0f;

constexpr float MiddleY    = 86.0f;
constexpr float SelectX    = ColX + 54.0f;
constexpr float StartX     = ColX + 67.0f;

// The six that are not buttons: two columns to the right of the controller.
constexpr float UtilX1 = ColX + 126.0f;
constexpr float UtilX2 = ColX + 142.0f;
constexpr float UtilY1 = 60.0f;
constexpr float UtilY2 = 75.0f;
constexpr float UtilY3 = 90.0f;

constexpr int   OutCols = 8;
constexpr float OutRow1 = 106.0f;
constexpr float OutRow2 = 121.0f;
inline constexpr float outX(int i) { return ColX + (ColW / OutCols) * (i + 0.5f); }

constexpr float LabelDrop = 5.9f;                // a label above its socket

} // namespace snes

// ---------------------------------------------------------------------------
// SAMPLER -- the cartridge's instruments
// ---------------------------------------------------------------------------
namespace sampler {

constexpr int   Hp     = 24;
constexpr float PanelW = Hp * HP;                // 121.92

constexpr int   Cols   = 6;
constexpr float Left   = 3.0f;
constexpr float Right  = PanelW - 3.0f;
constexpr float ColW   = (Right - Left) / Cols;
inline constexpr float colX(int i) { return Left + ColW * (i + 0.5f); }

// The bank, and the shape of the sample the knob is pointing at. A sampler
// whose instruments have no names -- and they have none, a SNES sample is a
// number -- has to show you the waveform or you are choosing blind.
constexpr float ScopeY = 15.5f;
constexpr float ScopeH = 24.0f;

// The rows below are set by the font: a label drawn out of pixels is about
// half as wide again as one that was typeset, and 2.4 mm of cap height is the
// smallest that survives Rack's own zoom. Everything here has room for its
// own label and nothing else's.
constexpr float ButtonY  = 45.5f;                // LOAD TAKE < >
constexpr float KnobRow1 = 58.5f;                // SAMPLE TUNE FINE PAN LEVEL ECHO
constexpr float KnobRow2 = 75.0f;                // A D S R  FEEDBACK  TIME
constexpr float ChoiceY  = 88.0f;                // LOOP and INTERPOLATION
constexpr float InRow    = 104.0f;               // V/OCT GATE SAMPLE LEVEL PAN TUNE
constexpr float OutRow   = 119.0f;               // MIX L MIX R VOICES DRY ECHO L ECHO R

constexpr float ChoiceW = 34.0f;
constexpr float ChoiceH = 7.0f;
constexpr float LabelDrop = 5.6f;

} // namespace sampler

// ---------------------------------------------------------------------------
// APU -- the sound chip alone
// ---------------------------------------------------------------------------
namespace apu {

constexpr int   Hp     = 20;
constexpr float PanelW = Hp * HP;                // 101.6

constexpr int   Cols   = 5;
constexpr float Left   = 3.0f;
constexpr float Right  = PanelW - 3.0f;
constexpr float ColW   = (Right - Left) / Cols;
inline constexpr float colX(int i) { return Left + ColW * (i + 0.5f); }

constexpr float PlateY = 15.5f;
constexpr float PlateH = 34.0f;                  // two lines and eight voices

constexpr float ButtonY  = 56.0f;                // LOAD TAKE RESET
constexpr float PortKnob = 71.0f;                // the four letterboxes
// A port's number goes between its knob and its socket rather than under
// each: under each puts two labels in the same 7 mm, and they land on top of
// one another.
constexpr float PortLabel = 80.0f;
constexpr float PortJack = 87.5f;

constexpr float RuleY    = 93.0f;
// RATE and VOLUME wear their labels above rather than below: below is where
// the output row's labels have to go, and there is only one 3 mm gap.
constexpr float WriteY   = 104.0f;
constexpr float OutRow   = 121.0f;               // MIX L MIX R VOICES V/OCT GATE

constexpr float ChoiceW = 30.0f;
constexpr float ChoiceH = 7.0f;
constexpr float LabelDrop = 5.6f;

} // namespace apu

// ---------------------------------------------------------------------------
// BENDER -- the circuit bending
// ---------------------------------------------------------------------------
namespace bender {

constexpr int   Hp     = 14;
constexpr float PanelW = Hp * HP;                // 71.12

constexpr float KnobX  = 20.0f;
constexpr float JackX  = 52.0f;

// Six sections in the 118 mm below the title. Each is a rule with its name on
// the left and what its socket wants on the right, then a knob and a jack.
// The cue lives up on the rule because down beside the socket there is no
// room for it before the next section starts.
constexpr float Section0 = 16.0f;
constexpr float SectionPitch = 19.0f;
inline constexpr float ruleY(int i)  { return Section0 + SectionPitch * i; }
inline constexpr float rowY(int i)   { return ruleY(i) + 12.0f; }

constexpr float ChoiceW = 40.0f;
constexpr float ChoiceH = 6.4f;

constexpr int SectionCount = 6;

} // namespace bender

} // namespace layout
} // namespace racksnes
