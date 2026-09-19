import { useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from "react";
import { type MonitorData, type ScheduleSettings } from "./lib/bridge";
import {
  type CurveShape,
  type SolarDay,
  MAX_OFFSET,
  MIN_GAP,
  computeSolarDays,
  DAY_RADIUS,
  scheduleAnchors,
  scheduleValueAt,
  formatMinutes,
  formatOffset,
} from "./lib/solar";
import { Button } from "./components/ui/button";
import { Checkbox } from "./components/ui/checkbox";
import { Input } from "./components/ui/input";
import { Label } from "./components/ui/label";
import { Slider } from "./components/ui/slider";

export const DEFAULT_OFFSETS = {
  dawnStartOffset: -30,
  dawnEndOffset: 30,
  duskStartOffset: -30,
  duskEndOffset: 30,
};

export function parseCoordinate(text: string, limit: number): number | null {
  const trimmed = text.trim();
  if (!trimmed) return null;
  const value = Number(trimmed);
  if (!Number.isFinite(value) || Math.abs(value) > limit) return null;
  return value;
}

function clamp(value: number, min: number, max: number) {
  return Math.min(max, Math.max(min, value));
}

// ── Graph ──────────────────────────────────────────────────────────────────

const VIEW_W = 440;
const VIEW_H = 184;
const PAD = { left: 34, right: 12, top: 24, bottom: 22 };
const PLOT_W = VIEW_W - PAD.left - PAD.right;
const PLOT_H = VIEW_H - PAD.top - PAD.bottom;
const PLOT_BOTTOM = PAD.top + PLOT_H;
const HANDLE_NAMES = ["Dawn starts", "Full brightness", "Evening starts", "Night level"];

interface SunCurveProps {
  shape: CurveShape;
  days: SolarDay[];
  now: Date;
  minLevel: number;
  onOffsetsChange: (offsets: Partial<CurveShape>) => void;
}

function SunCurve({ shape, days, now, minLevel, onOffsetsChange }: SunCurveProps) {
  const svgRef = useRef<SVGSVGElement>(null);
  const [dragging, setDragging] = useState<number | null>(null);
  const [hovered, setHovered] = useState<number | null>(null);
  const day = days[DAY_RADIUS];

  const x = (minute: number) => PAD.left + (clamp(minute, 0, 1440) / 1440) * PLOT_W;
  const y = (level: number) =>
    PAD.top + ((100 - clamp(level, minLevel, 100)) / (100 - minLevel)) * PLOT_H;

  const anchors = scheduleAnchors(shape, day);
  const anchorLevels = [shape.nightLevel, shape.dayLevel, shape.dayLevel, shape.nightLevel];

  const points: string[] = [];
  for (let m = 0; m <= 1440; m += 5) {
    points.push(`${x(m).toFixed(1)},${y(scheduleValueAt(shape, days, m)).toFixed(1)}`);
  }
  const linePath = `M${points.join("L")}`;
  const areaPath = `${linePath}L${x(1440).toFixed(1)},${PLOT_BOTTOM}L${x(0).toFixed(1)},${PLOT_BOTTOM}Z`;

  const nowMinutes = now.getHours() * 60 + now.getMinutes();
  const nowValue = scheduleValueAt(shape, days, nowMinutes);

  function minuteFromPointer(e: ReactPointerEvent) {
    const svg = svgRef.current;
    if (!svg) return 0;
    const rect = svg.getBoundingClientRect();
    const vx = ((e.clientX - rect.left) / rect.width) * VIEW_W;
    return Math.round((((vx - PAD.left) / PLOT_W) * 1440) / 5) * 5;
  }

  function startDrag(index: number, e: ReactPointerEvent<SVGCircleElement>) {
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
    setDragging(index);
  }

  function moveDrag(e: ReactPointerEvent) {
    if (dragging === null) return;
    const event = dragging < 2 ? day.sunrise : day.sunset;
    let minute = clamp(minuteFromPointer(e), event - MAX_OFFSET, event + MAX_OFFSET);
    if (dragging > 0) minute = Math.max(minute, anchors[dragging - 1] + MIN_GAP);
    if (dragging < 3) minute = Math.min(minute, anchors[dragging + 1] - MIN_GAP);
    const offset = clamp(minute - event, -MAX_OFFSET, MAX_OFFSET);
    const keys: (keyof CurveShape)[] = [
      "dawnStartOffset",
      "dawnEndOffset",
      "duskStartOffset",
      "duskEndOffset",
    ];
    if (shape[keys[dragging]] !== offset) onOffsetsChange({ [keys[dragging]]: offset });
  }

  function endDrag() {
    setDragging(null);
  }

  const active = dragging ?? hovered;
  const levelTicks = [100, Math.round((100 + minLevel) / 2), minLevel];
  if (minLevel < 0) levelTicks.splice(1, 1, 50, 0);
  const sunriseX = x(day.sunrise);
  const sunsetX = x(day.sunset);

  return (
    <svg
      ref={svgRef}
      viewBox={`0 0 ${VIEW_W} ${VIEW_H}`}
      className="w-full select-none touch-none"
      onPointerMove={moveDrag}
      onPointerUp={endDrag}
      onPointerCancel={endDrag}
    >
      <defs>
        <linearGradient id="ntb-dawn" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stopColor="#fef3c7" stopOpacity="0" />
          <stop offset="1" stopColor="#fef3c7" stopOpacity="0.9" />
        </linearGradient>
        <linearGradient id="ntb-dusk" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stopColor="#fef3c7" stopOpacity="0.9" />
          <stop offset="1" stopColor="#fef3c7" stopOpacity="0" />
        </linearGradient>
      </defs>

      <rect x={PAD.left} y={PAD.top} width={PLOT_W} height={PLOT_H} fill="#f8fafc" />
      {day.polar === 1 && (
        <rect x={PAD.left} y={PAD.top} width={PLOT_W} height={PLOT_H} fill="#fef3c7" opacity="0.9" />
      )}
      {day.polar === 0 && (
        <>
          <rect x={x(anchors[0])} y={PAD.top} width={Math.max(0, sunriseX - x(anchors[0]))} height={PLOT_H} fill="url(#ntb-dawn)" />
          <rect x={sunriseX} y={PAD.top} width={Math.max(0, sunsetX - sunriseX)} height={PLOT_H} fill="#fef3c7" opacity="0.9" />
          <rect x={sunsetX} y={PAD.top} width={Math.max(0, x(anchors[3]) - sunsetX)} height={PLOT_H} fill="url(#ntb-dusk)" />
        </>
      )}

      {levelTicks.map((level) => (
        <g key={level}>
          <line x1={PAD.left} x2={PAD.left + PLOT_W} y1={y(level)} y2={y(level)} stroke="#e5e5e5" strokeWidth="1" />
          <text x={PAD.left - 5} y={y(level) + 3} fontSize="9" fill="#a3a3a3" textAnchor="end">
            {level}%
          </text>
        </g>
      ))}
      {[0, 360, 720, 1080, 1440].map((minute) => (
        <g key={minute}>
          <line x1={x(minute)} x2={x(minute)} y1={PAD.top} y2={PLOT_BOTTOM} stroke="#e5e5e5" strokeWidth="1" />
          <text x={x(minute)} y={VIEW_H - 8} fontSize="9" fill="#a3a3a3" textAnchor={minute === 0 ? "start" : minute === 1440 ? "end" : "middle"}>
            {formatMinutes(minute)}
          </text>
        </g>
      ))}

      {day.polar === 0 && (
        <>
          <line x1={sunriseX} x2={sunriseX} y1={PAD.top - 2} y2={PLOT_BOTTOM} stroke="#f59e0b" strokeWidth="1" strokeDasharray="3 3" />
          <line x1={sunsetX} x2={sunsetX} y1={PAD.top - 2} y2={PLOT_BOTTOM} stroke="#f59e0b" strokeWidth="1" strokeDasharray="3 3" />
          <text x={sunriseX} y={PAD.top - 9} fontSize="9" fill="#b45309" textAnchor="middle">
            Sunrise {formatMinutes(day.sunrise)}
          </text>
          <text x={sunsetX} y={PAD.top - 9} fontSize="9" fill="#b45309" textAnchor="middle">
            Sunset {formatMinutes(day.sunset)}
          </text>
        </>
      )}

      <path d={areaPath} fill="#171717" opacity="0.08" />
      <path d={linePath} fill="none" stroke="#171717" strokeWidth="1.6" strokeLinejoin="round" />

      <line x1={x(nowMinutes)} x2={x(nowMinutes)} y1={PAD.top} y2={PLOT_BOTTOM} stroke="#2563eb" strokeWidth="1" strokeDasharray="2 3" />
      <circle cx={x(nowMinutes)} cy={y(nowValue)} r="3.5" fill="#2563eb" />
      <text
        x={x(nowMinutes) + (nowMinutes > 1200 ? -6 : 6)}
        y={y(nowValue) - 7}
        fontSize="9"
        fill="#2563eb"
        textAnchor={nowMinutes > 1200 ? "end" : "start"}
      >
        Now {nowValue}%
      </text>

      {day.polar === 0 &&
        anchors.map((anchor, index) => {
          const cx = x(anchor);
          const cy = y(anchorLevels[index]);
          const isActive = active === index;
          const event = index < 2 ? day.sunrise : day.sunset;
          return (
            <g key={index}>
              {isActive && (
                <text
                  x={cx}
                  y={cy - 12}
                  fontSize="9"
                  fill="#171717"
                  fontWeight="500"
                  textAnchor={cx < PAD.left + 70 ? "start" : cx > VIEW_W - 90 ? "end" : "middle"}
                >
                  {HANDLE_NAMES[index]} {formatMinutes(anchor)} ({formatOffset(anchor - event)})
                </text>
              )}
              <circle
                cx={cx}
                cy={cy}
                r={isActive ? 7 : 5.5}
                fill={dragging === index ? "#171717" : "#ffffff"}
                stroke="#171717"
                strokeWidth="1.6"
                style={{ cursor: "ew-resize" }}
                onPointerDown={(e) => startDrag(index, e)}
                onPointerEnter={() => setHovered(index)}
                onPointerLeave={() => setHovered(null)}
              />
            </g>
          );
        })}
    </svg>
  );
}

// ── Section ─────────────────────────────────────────────────────────────────

interface Props {
  settings: ScheduleSettings;
  onChange: (next: ScheduleSettings) => void;
  monitors: MonitorData[];
  minLevel: number;
  error: string;
}

export default function ScheduleSection({ settings, onChange, monitors, minLevel, error }: Props) {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    const id = window.setInterval(() => setNow(new Date()), 30000);
    return () => window.clearInterval(id);
  }, []);
  const latitude = parseCoordinate(settings.latitude, 90);
  const longitude = parseCoordinate(settings.longitude, 180);
  const hasLocation = latitude !== null && longitude !== null;
  const days = hasLocation ? computeSolarDays(latitude, longitude, now) : null;
  const day = days?.[DAY_RADIUS] ?? null;
  const shape: CurveShape = {
    dayLevel: clamp(settings.dayLevel, minLevel, 100),
    nightLevel: clamp(settings.nightLevel, minLevel, 100),
    dawnStartOffset: settings.dawnStartOffset,
    dawnEndOffset: settings.dawnEndOffset,
    duskStartOffset: settings.duskStartOffset,
    duskEndOffset: settings.duskEndOffset,
  };
  const anchors = day ? scheduleAnchors(shape, day) : null;

  function update(patch: Partial<ScheduleSettings>) {
    onChange({ ...settings, ...patch });
  }

  function toggleEnabled(enabled: boolean) {
    const patch: Partial<ScheduleSettings> = { enabled };
    if (enabled && !monitors.some((m) => settings.scheduledKeys.includes(m.key))) {
      patch.scheduledKeys = monitors.map((m) => m.key);
    }
    update(patch);
  }

  function toggleMonitor(key: string, on: boolean) {
    const set = new Set(settings.scheduledKeys);
    if (on) set.add(key);
    else set.delete(key);
    update({ scheduledKeys: Array.from(set) });
  }

  const resetTime = `${String(Math.floor(settings.cycleResetMinutes / 60)).padStart(2, "0")}:${String(settings.cycleResetMinutes % 60).padStart(2, "0")}`;

  return (
    <div className="space-y-3">
      <div className="flex items-start gap-2">
        <Checkbox
          id="scheduleEnabled"
          className="mt-0.5"
          checked={settings.enabled}
          onChange={(e) => toggleEnabled(e.target.checked)}
        />
        <div className="space-y-0.5">
          <Label htmlFor="scheduleEnabled" className="cursor-pointer">
            Adjust brightness automatically with the sun
          </Label>
          <p className="text-neutral-500 text-[11px] leading-snug">
            Brightens the selected monitors to the daytime level after sunrise
            and dims them to the night level after sunset, following the
            sunrise and sunset times for your location every day of the year.
            Saved with the Save button.
          </p>
        </div>
      </div>

      {settings.enabled && (
        <div className="ml-6 space-y-3">
          <div className="grid grid-cols-2 gap-3">
            <div className="space-y-1">
              <Label htmlFor="latitude">Latitude</Label>
              <Input
                id="latitude"
                inputMode="decimal"
                placeholder="e.g. 51.5074"
                value={settings.latitude}
                onChange={(e) => update({ latitude: e.target.value })}
                className={error && latitude === null ? "border-red-500" : ""}
              />
            </div>
            <div className="space-y-1">
              <Label htmlFor="longitude">Longitude</Label>
              <Input
                id="longitude"
                inputMode="decimal"
                placeholder="e.g. -0.1278"
                value={settings.longitude}
                onChange={(e) => update({ longitude: e.target.value })}
                className={error && longitude === null ? "border-red-500" : ""}
              />
            </div>
          </div>
          <p className="text-neutral-500 text-[11px] leading-snug -mt-1">
            Decimal degrees; north and east are positive. Any map service shows
            the coordinates of a place, and a nearby city is close enough.
          </p>
          {error && <p className="text-red-600 text-[11px]">{error}</p>}

          <div className="space-y-2">
            <div className="space-y-1">
              <div className="flex items-center justify-between">
                <Label htmlFor="dayLevel" className="text-xs">
                  Daytime brightness
                </Label>
                <span className="w-10 text-right text-xs tabular-nums">{shape.dayLevel}%</span>
              </div>
              <Slider
                id="dayLevel"
                min={minLevel}
                max={100}
                step={1}
                value={shape.dayLevel}
                onChange={(e) => update({ dayLevel: Number(e.target.value) })}
              />
            </div>
            <div className="space-y-1">
              <div className="flex items-center justify-between">
                <Label htmlFor="nightLevel" className="text-xs">
                  Night brightness
                </Label>
                <span className="w-10 text-right text-xs tabular-nums">{shape.nightLevel}%</span>
              </div>
              <Slider
                id="nightLevel"
                min={minLevel}
                max={100}
                step={1}
                value={shape.nightLevel}
                onChange={(e) => update({ nightLevel: Number(e.target.value) })}
              />
            </div>
          </div>

          <div className="space-y-1.5 rounded-md border border-neutral-200 px-2 pt-2 pb-1.5">
            {day && days ? (
              <>
                <SunCurve
                  shape={shape}
                  days={days}
                  now={now}
                  minLevel={minLevel}
                  onOffsetsChange={(offsets) => update(offsets)}
                />
                <div className="flex items-start justify-between gap-2 px-1">
                  <p className="text-neutral-500 text-[11px] leading-snug">
                    {day.polar === 1
                      ? "The sun does not set today, so the daytime level applies all day."
                      : day.polar === -1
                        ? "The sun does not rise today, so the night level applies all day."
                        : anchors && (
                            <>
                              Brightens {formatMinutes(anchors[0])}–{formatMinutes(anchors[1])}, dims{" "}
                              {formatMinutes(anchors[2])}–{formatMinutes(anchors[3])} today. Drag the
                              four points to change when the transitions happen; they stay tied to
                              sunrise and sunset as the seasons change.
                            </>
                          )}
                  </p>
                  <Button
                    variant="ghost"
                    size="sm"
                    className="h-6 shrink-0 px-1.5 text-[11px] text-neutral-500"
                    onClick={() => update(DEFAULT_OFFSETS)}
                  >
                    Reset curve
                  </Button>
                </div>
              </>
            ) : (
              <p className="px-1 py-6 text-center text-[11px] text-neutral-500">
                Enter your latitude and longitude to see today's curve.
              </p>
            )}
          </div>

          <div className="space-y-1">
            <Label className="text-xs">Apply to</Label>
            {monitors.length > 0 && !monitors.some((m) => settings.scheduledKeys.includes(m.key)) && (
              <p className="text-[11px] leading-snug text-amber-700">
                No monitor is selected, so the schedule will not change anything.
              </p>
            )}
            {monitors.length === 0 ? (
              <p className="text-neutral-500 text-[11px]">No monitors are available.</p>
            ) : (
              <div className="flex flex-wrap gap-x-4 gap-y-1">
                {monitors.map((m) => {
                  const id = `schedule-monitor-${m.uid}`;
                  return (
                    <div key={m.uid} className="flex items-center gap-2">
                      <Checkbox
                        id={id}
                        checked={settings.scheduledKeys.includes(m.key)}
                        onChange={(e) => toggleMonitor(m.key, e.target.checked)}
                      />
                      <Label htmlFor={id} className="cursor-pointer text-[11px] font-normal">
                        {m.name}
                      </Label>
                    </div>
                  );
                })}
              </div>
            )}
          </div>

          <div className="flex items-start gap-3">
            <div className="space-y-1">
              <Label htmlFor="cycleReset" className="text-xs">
                Cycle reset time
              </Label>
              <Input
                id="cycleReset"
                type="time"
                className="w-28"
                value={resetTime}
                onChange={(e) => {
                  const [h, m] = e.target.value.split(":").map(Number);
                  if (Number.isFinite(h) && Number.isFinite(m)) {
                    update({ cycleResetMinutes: clamp(h * 60 + m, 0, 1439) });
                  }
                }}
              />
            </div>
            <p className="pt-5 text-neutral-500 text-[11px] leading-snug">
              When you change a scheduled monitor's brightness by hand, the
              schedule leaves that monitor alone until this time of day, then
              takes over again.
            </p>
          </div>
        </div>
      )}
    </div>
  );
}
