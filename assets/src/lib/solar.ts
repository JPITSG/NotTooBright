// Sun-based brightness curve, mirroring the host's implementation so the
// preview in the dialog matches what gets applied.

export interface SolarDay {
  polar: 0 | 1 | -1; // 1: sun never sets, -1: sun never rises
  sunrise: number; // local minutes since midnight (may exceed 0..1439)
  sunset: number;
  noon: number;
}

export interface CurveShape {
  dayLevel: number;
  nightLevel: number;
  dawnStartOffset: number; // minutes relative to sunrise
  dawnEndOffset: number;
  duskStartOffset: number; // minutes relative to sunset
  duskEndOffset: number;
  deepSleepEnabled: boolean;
  deepSleepLevel: number;
  deepSleepMinutes: number; // time of day deep sleep starts fading in
}

export const MAX_OFFSET = 6 * 60;
export const MIN_GAP = 5;
export const DAY_RADIUS = 2;
// Minutes deep sleep takes to fade in.
export const DEEP_SLEEP_RAMP = 5;

const RAD = Math.PI / 180;

function julianDay(year: number, month: number, day: number) {
  if (month <= 2) {
    year -= 1;
    month += 12;
  }
  const a = Math.floor(year / 100);
  const b = 2 - a + Math.floor(a / 4);
  return (
    Math.floor(365.25 * (year + 4716)) +
    Math.floor(30.6001 * (month + 1)) +
    day +
    b -
    1524.5
  );
}

// NOAA solar position equations: solar noon (minutes after 0h UTC) and the
// sunrise/sunset hour angle. cosHa outside -1..1 means no sunrise/sunset.
function solarNoonAndHourAngle(latitude: number, longitude: number, jd: number) {
  const t = (jd - 2451545.0) / 36525.0;
  let l0 = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360;
  if (l0 < 0) l0 += 360;
  const m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
  const e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
  const mr = m * RAD;
  const c =
    Math.sin(mr) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
    Math.sin(2 * mr) * (0.019993 - 0.000101 * t) +
    Math.sin(3 * mr) * 0.000289;
  const trueLongitude = l0 + c;
  const omega = 125.04 - 1934.136 * t;
  const lambda = trueLongitude - 0.00569 - 0.00478 * Math.sin(omega * RAD);
  const epsilon0 =
    23 +
    (26 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60) / 60;
  const epsilon = epsilon0 + 0.00256 * Math.cos(omega * RAD);
  const declination = Math.asin(Math.sin(epsilon * RAD) * Math.sin(lambda * RAD));
  let y = Math.tan((epsilon * RAD) / 2);
  y *= y;
  const l0r = l0 * RAD;
  const equationOfTime =
    (4 / RAD) *
    (y * Math.sin(2 * l0r) -
      2 * e * Math.sin(mr) +
      4 * e * y * Math.sin(mr) * Math.cos(2 * l0r) -
      0.5 * y * y * Math.sin(4 * l0r) -
      1.25 * e * e * Math.sin(2 * mr));
  const noonUtcMinutes = 720 - 4 * longitude - equationOfTime;
  const latr = latitude * RAD;
  const cosHa =
    Math.cos(90.833 * RAD) / (Math.cos(latr) * Math.cos(declination)) -
    Math.tan(latr) * Math.tan(declination);
  const hourAngle = cosHa >= -1 && cosHa <= 1 ? Math.acos(cosHa) / RAD : 0;
  return { noonUtcMinutes, cosHa, hourAngle };
}

// Minutes after local midnight of `date` for "0h UTC of that calendar date
// plus utcMinutes"; the JS engine applies the local time zone and DST.
function utcMinutesToLocalMinutes(date: Date, utcMinutes: number) {
  const instant = new Date(
    Date.UTC(date.getFullYear(), date.getMonth(), date.getDate()) + utcMinutes * 60000
  );
  // Use calendar dates and wall-clock fields, matching the host. Elapsed
  // time since local midnight differs by an hour on DST transition days.
  const calendarDay = Date.UTC(instant.getFullYear(), instant.getMonth(), instant.getDate());
  const referenceDay = Date.UTC(date.getFullYear(), date.getMonth(), date.getDate());
  return ((calendarDay - referenceDay) / 86400000) * 1440 +
    instant.getHours() * 60 + instant.getMinutes();
}

export function computeSolarDay(
  latitude: number,
  longitude: number,
  date: Date
): SolarDay {
  const jd = julianDay(date.getFullYear(), date.getMonth() + 1, date.getDate());
  const { noonUtcMinutes, cosHa, hourAngle } = solarNoonAndHourAngle(
    latitude,
    longitude,
    jd
  );
  const noon = utcMinutesToLocalMinutes(date, noonUtcMinutes);
  if (cosHa > 1) return { polar: -1, sunrise: noon, sunset: noon, noon };
  if (cosHa < -1) return { polar: 1, sunrise: noon, sunset: noon, noon };
  return {
    polar: 0,
    sunrise: utcMinutesToLocalMinutes(date, noonUtcMinutes - hourAngle * 4),
    sunset: utcMinutesToLocalMinutes(date, noonUtcMinutes + hourAngle * 4),
    noon,
  };
}

// Keep the neighbouring dates' real anchors when a transition crosses midnight.
export function computeSolarDays(latitude: number, longitude: number, date: Date) {
  return Array.from({ length: DAY_RADIUS * 2 + 1 }, (_, i) =>
    computeSolarDay(latitude, longitude, new Date(
      date.getFullYear(), date.getMonth(), date.getDate() + i - DAY_RADIUS, 12
    ))
  );
}

// The four transition anchors in local minutes, kept in order.
export function scheduleAnchors(shape: CurveShape, day: SolarDay) {
  const anchors = [
    day.sunrise + shape.dawnStartOffset,
    day.sunrise + shape.dawnEndOffset,
    day.sunset + shape.duskStartOffset,
    day.sunset + shape.duskEndOffset,
  ];
  for (let i = 1; i < 4; i++) {
    if (anchors[i] < anchors[i - 1] + MIN_GAP) anchors[i] = anchors[i - 1] + MIN_GAP;
  }
  return anchors;
}

function smoothStep(x: number) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  return x * x * (3 - 2 * x);
}

// The morning that ends a deep sleep, in minutes relative to today's
// midnight: the end of the dawn transition (the dawn ramp rises from the
// deep sleep level), the start of a day the sun never sets, or the solar
// noon of one it never rises. `rise` is where the morning's ramp begins.
function morningOf(shape: CurveShape, day: SolarDay, index: number) {
  const base = (index - DAY_RADIUS) * 1440;
  if (day.polar > 0) return { morning: base, rise: base };
  if (day.polar < 0) return { morning: base + day.noon, rise: base + day.noon };
  const a = scheduleAnchors(shape, day);
  return { morning: base + a[1], rise: base + a[0] };
}

// How far deep sleep has taken over (0..1) at a local time, as on the host.
export function deepSleepAt(shape: CurveShape, days: readonly SolarDay[], minutes: number) {
  if (!shape.deepSleepEnabled) return 0;
  let start = shape.deepSleepMinutes;
  if (start > minutes) start -= 1440; // it began yesterday
  for (let i = 0; i < days.length; i++) {
    const { morning } = morningOf(shape, days[i], i);
    if (morning > start && morning <= minutes) return 0; // that morning has come
  }
  return smoothStep((minutes - start) / DEEP_SLEEP_RAMP);
}

// Today's deep sleep stretches for the graph: from the start of each one
// that touches today until its morning ramp begins, clipped to the day.
export function deepSleepSpans(shape: CurveShape, days: readonly SolarDay[]) {
  const spans: [number, number][] = [];
  if (!shape.deepSleepEnabled) return spans;
  for (const start of [shape.deepSleepMinutes - 1440, shape.deepSleepMinutes]) {
    let end = Infinity;
    for (let i = 0; i < days.length; i++) {
      const { morning, rise } = morningOf(shape, days[i], i);
      if (morning > start) {
        end = rise;
        break;
      }
    }
    const from = Math.max(start, 0);
    const to = Math.min(end, 1440);
    if (to > from) spans.push([from, to]);
  }
  return spans;
}

export function scheduleValueAt(
  shape: CurveShape,
  days: readonly SolarDay[],
  minutes: number
) {
  const today = days[DAY_RADIUS];
  let daylight = 0;
  if (today.polar) {
    daylight = today.polar > 0 ? 1 : 0;
  } else {
    for (let i = 0; i < days.length; i++) {
      if (days[i].polar) continue;
      const a = scheduleAnchors(shape, days[i]);
      const t = minutes - (i - DAY_RADIUS) * 1440;
      let weight = 0;
      if (t > a[0] && t < a[3]) {
        if (t < a[1]) weight = smoothStep((t - a[0]) / (a[1] - a[0]));
        else if (t <= a[2]) weight = 1;
        else weight = 1 - smoothStep((t - a[2]) / (a[3] - a[2]));
      }
      daylight = Math.max(daylight, weight);
    }
  }
  // Deep sleep replaces the night level; the same order of operations as
  // the host keeps the rounding identical.
  const base = shape.nightLevel +
    (shape.deepSleepLevel - shape.nightLevel) * deepSleepAt(shape, days, minutes);
  const value = base + (shape.dayLevel - base) * daylight;
  // Match C lround for negative half-integers too.
  return Math.sign(value) * Math.floor(Math.abs(value) + 0.5);
}

export function formatMinutes(minutes: number) {
  const m = ((Math.round(minutes) % 1440) + 1440) % 1440;
  const h = Math.floor(m / 60);
  return `${String(h).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
}

export function formatOffset(minutes: number) {
  const sign = minutes < 0 ? "−" : "+";
  const abs = Math.abs(minutes);
  return `${sign}${Math.floor(abs / 60)}:${String(abs % 60).padStart(2, "0")}`;
}
