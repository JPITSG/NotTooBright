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
}

export const MAX_OFFSET = 6 * 60;
export const MIN_GAP = 5;

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
  const instant =
    Date.UTC(date.getFullYear(), date.getMonth(), date.getDate()) +
    utcMinutes * 60000;
  const localMidnight = new Date(
    date.getFullYear(),
    date.getMonth(),
    date.getDate()
  ).getTime();
  return Math.round((instant - localMidnight) / 60000);
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

export function scheduleValueAt(
  shape: CurveShape,
  day: SolarDay,
  minutes: number
) {
  if (day.polar > 0) return shape.dayLevel;
  if (day.polar < 0) return shape.nightLevel;
  const a = scheduleAnchors(shape, day);
  let t = minutes;
  for (const candidate of [minutes, minutes + 1440, minutes - 1440]) {
    if (candidate >= a[0] && candidate <= a[3]) {
      t = candidate;
      break;
    }
  }
  const { nightLevel: night, dayLevel: dayLevel } = shape;
  let v: number;
  if (t <= a[0] || t >= a[3]) v = night;
  else if (t < a[1]) v = night + (dayLevel - night) * smoothStep((t - a[0]) / (a[1] - a[0]));
  else if (t <= a[2]) v = dayLevel;
  else v = dayLevel + (night - dayLevel) * smoothStep((t - a[2]) / (a[3] - a[2]));
  return Math.round(v);
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
