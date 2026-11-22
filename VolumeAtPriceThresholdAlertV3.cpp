#include "sierrachart.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <vector>

SCDLLName("Volume At Price Threshold Alert V3")

namespace
{
constexpr int MaxCircleDiameter = 100;
constexpr int TriggerSubgraphCount = SC_SUBGRAPHS_AVAILABLE - 13;
constexpr int CountSubgraphIndex = SC_SUBGRAPHS_AVAILABLE - 2;
constexpr int ExtensionSubgraphIndex = SC_SUBGRAPHS_AVAILABLE - 1;
constexpr int StateKey = 1;

enum InputIndex
{
    ComparisonMethod = 0,
    VolumeThreshold = 1,
    AlertNumber = 2,
    DrawExtensionLines = 3,
    DrawTransparentExtensionRange = 4,
    PercentageThreshold = 5,
    AdditionalVolumeThreshold = 6,
    AllowZeroCompares = 7,
    DivideByZeroAction = 8,
    ExtendToEndOfChart = 10,
    NumberOfDaysToCalculate = 11,
    MinimumVolumeForRatios = 13,
    BiggestCircleSize = 14,
    SmallestCircleSize = 15,
    BullishColor = 16,
    BullishTransparency = 17,
    BearishColor = 18,
    BearishTransparency = 19,
};

enum Comparison
{
    BidVolume, AskVolume, TotalVolume, NumberOfTrades, Difference,
    DiagonalDifference, Ratio, DiagonalRatio, BidAndAsk,
};

struct Circle
{
    t_ChartArrayDataType Price;
    double Delta;
};

struct BarState
{
    std::vector<Circle> Circles;
    double Smallest = 0;
    double Biggest = 0;
    int LineCount = 0;
    int TriggerCount = 0;
};

struct StudyState
{
    std::vector<BarState> Bars;
    // Two entries per populated bar let live updates replace extrema in logarithmic time.
    std::multiset<double> BarExtrema;
    int FirstOutputBar = 0;
};

bool MeetsVolumeThreshold(double Volume, unsigned int Threshold)
{
    return Threshold == 0 ? Volume == 0 : Volume >= Threshold;
}

bool MeetsSignedThreshold(double Value, int Threshold)
{
    return (Threshold > 0 && Value >= Threshold) || (Threshold < 0 && Value <= Threshold);
}

struct ComparisonSettings
{
    unsigned int Method;
    int VolumeThreshold;
    unsigned int AdditionalVolumeThreshold;
    int PercentageThreshold;
    bool AllowZeroCompares;
    unsigned int DivideByZeroAction;
    unsigned int MinimumVolumeForRatios;
};

bool IsTriggered(SCStudyInterfaceRef sc, const ComparisonSettings& Settings,
    const s_VolumeAtPriceV2& Level, const s_VolumeAtPriceV2* NextLevel, double Bid, double Ask)
{
    const auto Threshold = static_cast<unsigned int>(Settings.VolumeThreshold);
    switch (Settings.Method)
    {
    case BidVolume:
        return MeetsVolumeThreshold(Bid, Threshold);
    case AskVolume:
        return MeetsVolumeThreshold(Ask, Threshold);
    case TotalVolume:
        return MeetsVolumeThreshold(Level.GetVolume(), Threshold);
    case NumberOfTrades:
        return MeetsVolumeThreshold(Level.NumberOfTrades, Threshold);
    case BidAndAsk:
        return MeetsVolumeThreshold(Bid, Threshold)
            && MeetsVolumeThreshold(Ask, Settings.AdditionalVolumeThreshold);
    case Difference:
    case DiagonalDifference:
        return MeetsSignedThreshold(Ask - Bid, Settings.VolumeThreshold);
    case Ratio:
    case DiagonalRatio:
    {
        const double Larger = (std::max)(Ask, Bid);
        const double Smaller = (std::min)(Ask, Bid);
        if ((Smaller == 0 && !Settings.AllowZeroCompares)
            || Level.GetVolume() < Settings.MinimumVolumeForRatios
            || (NextLevel != nullptr && NextLevel->GetVolume() < Settings.MinimumVolumeForRatios))
            return false;

        const double Percent = Smaller == 0
            ? (Settings.DivideByZeroAction == 0 ? std::trunc(Larger * 100) : 1000)
            : sc.Round(Larger / Smaller * 100);
        return MeetsSignedThreshold(Ask >= Bid ? Percent : -Percent, Settings.PercentageThreshold);
    }
    }

    return false;
}

int CircleDiameter(double Magnitude, double Smallest, double Biggest,
    int SmallestDiameter, int BiggestDiameter)
{
    if (Smallest == Biggest)
        return SmallestDiameter;

    return SmallestDiameter + static_cast<int>(std::lround(
        (Magnitude - Smallest) / (Biggest - Smallest) * (BiggestDiameter - SmallestDiameter)));
}

struct CircleSpan
{
    int Left;
    int Top;
    int Bottom;
};

std::vector<CircleSpan> MakeCircleSpans(int Diameter)
{
    // Combine consecutive rows of the same width so Sierra Chart blends each pixel once.
    std::vector<CircleSpan> Spans;
    for (int Row = 0; Row < Diameter; ++Row)
    {
        const int Dy = 2 * Row + 1 - Diameter;
        const double HalfWidth = std::sqrt(static_cast<double>(Diameter * Diameter - Dy * Dy));
        const int Left = static_cast<int>(std::ceil((Diameter - 1 - HalfWidth) / 2));
        if (!Spans.empty() && Spans.back().Left == Left)
            Spans.back().Bottom = Row + 1;
        else
            Spans.push_back({Left, Row, Row + 1});
    }
    return Spans;
}

void DrawCircles(HWND, HDC, SCStudyInterfaceRef sc)
{
    const auto* State = static_cast<const StudyState*>(sc.GetPersistentPointer(StateKey));
    if (State == nullptr || State->BarExtrema.empty() || sc.HideStudy)
        return;

    const int FirstBar = (std::max)(State->FirstOutputBar, sc.IndexOfFirstVisibleBar);
    const int LastBar = (std::min)(sc.IndexOfLastVisibleBar, static_cast<int>(State->Bars.size()) - 1);
    const double Smallest = *State->BarExtrema.begin();
    const double Biggest = *State->BarExtrema.rbegin();
    const int SmallSize = std::clamp(sc.Input[SmallestCircleSize].GetInt(), 1, MaxCircleDiameter);
    const int BigSize = std::clamp(sc.Input[BiggestCircleSize].GetInt(), 1, MaxCircleDiameter);
    const int SmallestDiameter = (std::min)(SmallSize, BigSize);
    const int BiggestDiameter = (std::max)(SmallSize, BigSize);

    n_ACSIL::s_GraphicsColor Bullish;
    Bullish.SetColorValue(sc.Input[BullishColor].GetColor());
    n_ACSIL::s_GraphicsColor Bearish;
    Bearish.SetColorValue(sc.Input[BearishColor].GetColor());
    const int BullishTransparencySetting = std::clamp(sc.Input[BullishTransparency].GetInt(), 0, 100);
    const int BearishTransparencySetting = std::clamp(sc.Input[BearishTransparency].GetInt(), 0, 100);
    std::array<std::vector<CircleSpan>, MaxCircleDiameter + 1> Shapes;

    for (int BarIndex = FirstBar; BarIndex <= LastBar; ++BarIndex)
    {
        const auto& Circles = State->Bars[BarIndex].Circles;
        if (Circles.empty())
            continue;

        const int X = sc.BarIndexToXPixelCoordinate(BarIndex);
        for (const Circle& Mark : Circles)
        {
            const bool IsBullish = Mark.Delta > 0;
            const int Transparency = IsBullish ? BullishTransparencySetting : BearishTransparencySetting;
            if (Transparency >= 100)
                continue;

            const int Diameter = CircleDiameter(std::abs(Mark.Delta), Smallest, Biggest,
                SmallestDiameter, BiggestDiameter);
            const int Y = sc.RegionValueToYPixelCoordinate(Mark.Price, sc.GraphRegion);
            const int Left = X - Diameter / 2;
            const int Top = Y - Diameter / 2;
            auto& Spans = Shapes[Diameter];
            if (Spans.empty())
                Spans = MakeCircleSpans(Diameter);

            const auto& Color = IsBullish ? Bullish : Bearish;
            for (const CircleSpan& Span : Spans)
            {
                n_ACSIL::s_GraphicsRectangle Rect;
                Rect.Left = (std::max)(Left + Span.Left, sc.StudyRegionLeftCoordinate);
                Rect.Right = (std::min)(Left + Diameter - Span.Left, sc.StudyRegionRightCoordinate);
                Rect.Top = (std::max)(Top + Span.Top, sc.StudyRegionTopCoordinate);
                Rect.Bottom = (std::min)(Top + Span.Bottom, sc.StudyRegionBottomCoordinate);
                if (Rect.Left < Rect.Right && Rect.Top < Rect.Bottom)
                    sc.Graphics.FillRectangleWithColorTransparent(Rect, Color, static_cast<uint8_t>(Transparency));
            }
        }
    }
}

void RemoveBarExtrema(StudyState& State, const BarState& Bar)
{
    if (Bar.Circles.empty())
        return;

    State.BarExtrema.erase(State.BarExtrema.find(Bar.Smallest));
    State.BarExtrema.erase(State.BarExtrema.find(Bar.Biggest));
}

void ClearBarOutputs(SCStudyInterfaceRef sc, int BarIndex, BarState& Bar,
    int NextTriggerCount, int NextLineCount)
{
    for (int Trigger = NextTriggerCount; Trigger < Bar.TriggerCount; ++Trigger)
        sc.Subgraph[Trigger][BarIndex] = 0;
    for (int LineID = NextLineCount; LineID < Bar.LineCount; ++LineID)
        sc.DeleteLineUntilFutureIntersection(BarIndex, LineID);
    Bar.TriggerCount = NextTriggerCount;
    Bar.LineCount = NextLineCount;
}

void AddExtensionLine(SCStudyInterfaceRef sc, int BarIndex, int LineID,
    t_ChartArrayDataType Price, int Transparency)
{
    const SCSubgraphRef Properties = sc.Subgraph[ExtensionSubgraphIndex];
    n_ACSIL::s_LineUntilFutureIntersection Line;
    Line.StartBarIndex = BarIndex;
    Line.LineIDForBar = LineID;
    Line.LineValue = Price;
    if (sc.Input[DrawTransparentExtensionRange].GetYesNo())
    {
        Line.LineValue = Price + sc.TickSize * 0.5;
        Line.UseLineValue2 = true;
        Line.LineValue2ForRange = Price - sc.TickSize * 0.5;
        Line.TransparencyLevel = Transparency;
    }
    Line.LineColor = Properties.PrimaryColor;
    Line.LineWidth = Properties.LineWidth;
    Line.LineStyle = Properties.LineStyle;
    Line.AlwaysExtendToEndOfChart = sc.Input[ExtendToEndOfChart].GetYesNo();
    sc.AddLineUntilFutureIntersectionEx(Line);
}
}

SCSFExport scsf_VolumeAtPriceThresholdAlertV3(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName = "Volume At Price Threshold Alert V3";
        sc.StudyDescription = "Marks qualifying ask-bid imbalances with circles sized by absolute volume difference across loaded chart data.";
        sc.GraphRegion = 0;
        sc.AutoLoop = 0;
        sc.ValueFormat = VALUEFORMAT_INHERITED;
        sc.MaintainVolumeAtPriceData = 1;

        // Retain trigger prices and alert counts as study outputs. The drawing callback renders the markers.
        for (int Index = 0; Index < TriggerSubgraphCount; ++Index)
        {
            auto& Trigger = sc.Subgraph[Index];
            Trigger.Name.Format("Trigger %d", Index);
            Trigger.DrawStyle = DRAWSTYLE_IGNORE;
            Trigger.DrawZeros = 0;
            Trigger.DisplayNameValueInWindowsFlags = 0;
        }
        auto& Count = sc.Subgraph[CountSubgraphIndex];
        Count.Name = "Count of Alerts";
        Count.DrawStyle = DRAWSTYLE_IGNORE;
        Count.PrimaryColor = RGB(0, 255, 0);
        Count.DrawZeros = 1;

        auto& Extension = sc.Subgraph[ExtensionSubgraphIndex];
        Extension.Name = "Extension Line Properties";
        Extension.DrawStyle = DRAWSTYLE_SUBGRAPH_NAME_AND_VALUE_LABELS_ONLY;
        Extension.LineWidth = 1;
        Extension.PrimaryColor = RGB(255, 0, 255);
        Extension.DrawZeros = 0;
        Extension.DisplayNameValueInWindowsFlags = 0;
        int DisplayOrder = 1;

        sc.Input[ComparisonMethod].Name = "Comparison Method";
        sc.Input[ComparisonMethod].SetCustomInputStrings("Bid Volume;Ask Volume;Total Volume;Number of Trades;Ask Volume Bid Volume Difference;Ask Volume Bid Volume Diagonal Difference;Ask Volume Bid Volume Ratio;Ask Volume Bid Volume Diagonal Ratio;Bid Volume and Ask Volume Separately");
        sc.Input[ComparisonMethod].SetCustomInputIndex(2);
        sc.Input[ComparisonMethod].DisplayOrder = DisplayOrder++;

        sc.Input[VolumeThreshold].Name = "Volume Threshold";
        sc.Input[VolumeThreshold].SetInt(100);
        sc.Input[VolumeThreshold].DisplayOrder = DisplayOrder++;

        sc.Input[AlertNumber].Name = "Volume Alert Number";
        sc.Input[AlertNumber].SetAlertSoundNumber(0);
        sc.Input[AlertNumber].DisplayOrder = DisplayOrder++;

        sc.Input[DrawExtensionLines].Name = "Draw Extension Lines";
        sc.Input[DrawExtensionLines].SetCustomInputStrings("None;All Alerts");
        sc.Input[DrawExtensionLines].SetCustomInputIndex(0);
        sc.Input[DrawExtensionLines].DisplayOrder = DisplayOrder++;

        sc.Input[DrawTransparentExtensionRange].Name = "Draw Extension Lines With Transparent Range";
        sc.Input[DrawTransparentExtensionRange].SetYesNo(false);
        sc.Input[DrawTransparentExtensionRange].DisplayOrder = DisplayOrder++;

        sc.Input[PercentageThreshold].Name = "Percentage Threshold";
        sc.Input[PercentageThreshold].SetInt(150);
        sc.Input[PercentageThreshold].DisplayOrder = DisplayOrder++;

        sc.Input[MinimumVolumeForRatios].Name = "Minimum Volume Value for Ratio Comparisons";
        sc.Input[MinimumVolumeForRatios].SetInt(0);
        sc.Input[MinimumVolumeForRatios].DisplayOrder = DisplayOrder++;

        sc.Input[AdditionalVolumeThreshold].Name = "Additional Volume Threshold";
        sc.Input[AdditionalVolumeThreshold].SetInt(100);
        sc.Input[AdditionalVolumeThreshold].DisplayOrder = DisplayOrder++;

        sc.Input[AllowZeroCompares].Name = "Enable Zero Bid/Ask Compares";
        sc.Input[AllowZeroCompares].SetYesNo(0);
        sc.Input[AllowZeroCompares].DisplayOrder = DisplayOrder++;

        sc.Input[DivideByZeroAction].Name = "Zero Value Compare Action";
        sc.Input[DivideByZeroAction].SetCustomInputStrings("Set 0 to 1;Set Percentage to +/- 1000%");
        sc.Input[DivideByZeroAction].SetCustomInputIndex(0);
        sc.Input[DivideByZeroAction].DisplayOrder = DisplayOrder++;

        sc.Input[ExtendToEndOfChart].Name = "Draw Extension Lines until End of Chart";
        sc.Input[ExtendToEndOfChart].SetYesNo(false);
        sc.Input[ExtendToEndOfChart].DisplayOrder = DisplayOrder++;

        sc.Input[NumberOfDaysToCalculate].Name = "Number of Days to Calculate";
        sc.Input[NumberOfDaysToCalculate].SetInt(30);
        sc.Input[NumberOfDaysToCalculate].SetIntLimits(1, 10000);
        sc.Input[NumberOfDaysToCalculate].DisplayOrder = DisplayOrder++;

        sc.Input[BiggestCircleSize].Name = "Biggest Imbalance Pixel Size";
        sc.Input[BiggestCircleSize].SetInt(20);
        sc.Input[BiggestCircleSize].SetIntLimits(1, MaxCircleDiameter);
        sc.Input[BiggestCircleSize].DisplayOrder = DisplayOrder++;

        sc.Input[SmallestCircleSize].Name = "Smallest Imbalance Pixel Size";
        sc.Input[SmallestCircleSize].SetInt(5);
        sc.Input[SmallestCircleSize].SetIntLimits(1, MaxCircleDiameter);
        sc.Input[SmallestCircleSize].DisplayOrder = DisplayOrder++;

        sc.Input[BullishColor].Name = "Bullish Imbalance Circle Color";
        sc.Input[BullishColor].SetColor(RGB(0, 200, 0));
        sc.Input[BullishColor].DisplayOrder = DisplayOrder++;

        sc.Input[BullishTransparency].Name = "Bullish Imbalance Circle Transparency";
        sc.Input[BullishTransparency].SetInt(50);
        sc.Input[BullishTransparency].SetIntLimits(0, 100);
        sc.Input[BullishTransparency].DisplayOrder = DisplayOrder++;

        sc.Input[BearishColor].Name = "Bearish Imbalance Circle Color";
        sc.Input[BearishColor].SetColor(RGB(230, 0, 0));
        sc.Input[BearishColor].DisplayOrder = DisplayOrder++;

        sc.Input[BearishTransparency].Name = "Bearish Imbalance Circle Transparency";
        sc.Input[BearishTransparency].SetInt(50);
        sc.Input[BearishTransparency].SetIntLimits(0, 100);
        sc.Input[BearishTransparency].DisplayOrder = DisplayOrder++;
        return;
    }

    auto* State = static_cast<StudyState*>(sc.GetPersistentPointer(StateKey));
    if (sc.LastCallToFunction)
    {
        sc.p_GDIFunction = nullptr;
        delete State;
        sc.SetPersistentPointer(StateKey, nullptr);
        return;
    }

    if (sc.ArraySize == 0 || sc.VolumeAtPriceForBars == nullptr
        || sc.VolumeAtPriceForBars->GetNumberOfBars() < static_cast<unsigned int>(sc.ArraySize))
    {
        sc.p_GDIFunction = nullptr;
        return;
    }
    sc.p_GDIFunction = DrawCircles;

    const int PreviousSize = State == nullptr ? 0 : static_cast<int>(State->Bars.size());
    const bool Rebuild = State == nullptr || sc.IsFullRecalculation || sc.ArraySize < PreviousSize;
    if (State == nullptr)
    {
        State = new StudyState;
        sc.SetPersistentPointer(StateKey, State);
    }
    if (sc.IsFullRecalculation)
    {
        // Sierra Chart clears extension lines on a full recalculation.
        State->BarExtrema.clear();
    }
    else if (sc.ArraySize < PreviousSize)
    {
        for (int BarIndex = sc.ArraySize; BarIndex < PreviousSize; ++BarIndex)
            RemoveBarExtrema(*State, State->Bars[BarIndex]);
    }
    State->Bars.resize(sc.ArraySize);

    SCDateTime StartDate = sc.BaseDateTimeIn[sc.ArraySize - 1];
    StartDate.SubtractDays(sc.Input[NumberOfDaysToCalculate].GetInt());
    StartDate = sc.GetStartOfPeriodForDateTime(StartDate, TIME_PERIOD_LENGTH_UNIT_DAYS, 1, 0);
    int FirstOutputBar = (std::min)(State->FirstOutputBar, sc.ArraySize);
    while (FirstOutputBar > 0 && sc.BaseDateTimeIn[FirstOutputBar - 1] >= StartDate)
        --FirstOutputBar;
    while (FirstOutputBar < sc.ArraySize && sc.BaseDateTimeIn[FirstOutputBar] < StartDate)
        ++FirstOutputBar;
    for (int BarIndex = State->FirstOutputBar; BarIndex < FirstOutputBar; ++BarIndex)
    {
        BarState& Bar = State->Bars[BarIndex];
        if (sc.IsFullRecalculation)
            Bar.LineCount = 0;
        ClearBarOutputs(sc, BarIndex, Bar, 0, 0);
        sc.Subgraph[CountSubgraphIndex][BarIndex] = 0;
    }

    int UpdateStart = Rebuild ? 0 : (std::min)(sc.UpdateStartIndex, PreviousSize);
    if (FirstOutputBar < State->FirstOutputBar)
        UpdateStart = (std::min)(UpdateStart, FirstOutputBar);
    State->FirstOutputBar = FirstOutputBar;

    const ComparisonSettings Settings{
        sc.Input[ComparisonMethod].GetIndex(),
        sc.Input[VolumeThreshold].GetInt(),
        static_cast<unsigned int>(sc.Input[AdditionalVolumeThreshold].GetInt()),
        sc.Input[PercentageThreshold].GetInt(),
        sc.Input[AllowZeroCompares].GetYesNo() != 0,
        sc.Input[DivideByZeroAction].GetIndex(),
        static_cast<unsigned int>(sc.Input[MinimumVolumeForRatios].GetInt()),
    };
    const bool IsDiagonal = Settings.Method == DiagonalDifference || Settings.Method == DiagonalRatio;
    const int ExtensionTransparency = sc.GetChartStudyTransparencyLevel(sc.ChartNumber, sc.StudyGraphInstanceID);
    const int SoundNumber = sc.Input[AlertNumber].GetAlertSoundNumber();
    const bool EnableAlerts = SoundNumber > 0 && !sc.IsFullRecalculation
        && !sc.ChartIsDownloadingHistoricalData(sc.ChartNumber);

    for (int BarIndex = UpdateStart; BarIndex < sc.ArraySize; ++BarIndex)
    {
        BarState& Bar = State->Bars[BarIndex];
        if (sc.IsFullRecalculation)
            Bar.LineCount = 0;
        else
            RemoveBarExtrema(*State, Bar);
        Bar.Circles.clear();

        int AlertCount = 0;
        int TriggerCount = 0;
        int LineCount = 0;
        const bool WithinOutputWindow = BarIndex >= FirstOutputBar;
        const bool DrawLines = WithinOutputWindow && sc.Input[DrawExtensionLines].GetIndex() == 1
            && sc.GetBarHasClosedStatus(BarIndex) == BHCS_BAR_HAS_CLOSED;
        const int LevelCount = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(BarIndex);
        for (int PriceIndex = 0; PriceIndex < LevelCount; ++PriceIndex)
        {
            const s_VolumeAtPriceV2* Level = nullptr;
            if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(BarIndex, PriceIndex, &Level))
                continue;

            const s_VolumeAtPriceV2* AskLevel = Level;
            if (IsDiagonal && (PriceIndex + 1 >= LevelCount
                || !sc.VolumeAtPriceForBars->GetVAPElementAtIndex(BarIndex, PriceIndex + 1, &AskLevel)))
                continue;

            const double Bid = Level->GetBidVolume();
            const double Ask = AskLevel->GetAskVolume();
            if (!IsTriggered(sc, Settings, *Level, IsDiagonal ? AskLevel : nullptr, Bid, Ask))
                continue;

            const double Delta = Ask - Bid;
            const bool UseAskPrice = Delta > 0 || (Delta == 0 && Settings.Method == DiagonalRatio);
            const t_ChartArrayDataType Price = (UseAskPrice ? AskLevel : Level)->PriceInTicks * sc.TickSize;
            if (Delta != 0)
            {
                const double Magnitude = std::abs(Delta);
                if (Bar.Circles.empty())
                    Bar.Smallest = Bar.Biggest = Magnitude;
                else
                {
                    Bar.Smallest = (std::min)(Bar.Smallest, Magnitude);
                    Bar.Biggest = (std::max)(Bar.Biggest, Magnitude);
                }
                Bar.Circles.push_back({Price, Delta});
            }

            if (!WithinOutputWindow)
                continue;

            ++AlertCount;
            if (TriggerCount < TriggerSubgraphCount)
            {
                // V2's diagonal difference trigger subgraph used the lower price.
                sc.Subgraph[TriggerCount++][BarIndex] = Settings.Method == DiagonalDifference
                    ? Level->PriceInTicks * sc.TickSize : Price;
            }
            if (DrawLines)
                AddExtensionLine(sc, BarIndex, LineCount++, Price, ExtensionTransparency);
            if (EnableAlerts && BarIndex == sc.ArraySize - 1)
                sc.SetAlert(SoundNumber - 1, "Volume threshold triggered");
        }

        if (WithinOutputWindow)
        {
            sc.Subgraph[CountSubgraphIndex][BarIndex] = AlertCount;
            ClearBarOutputs(sc, BarIndex, Bar, TriggerCount, LineCount);
        }
        if (!Bar.Circles.empty())
        {
            State->BarExtrema.insert(Bar.Smallest);
            State->BarExtrema.insert(Bar.Biggest);
        }
    }
}
