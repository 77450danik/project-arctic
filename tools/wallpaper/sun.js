// Where the sun stands over Longyearbyen at a moment: elevation and azimuth in
// degrees (NOAA's solar position equations, good to a few hundredths of a
// degree, enough for light and shadows). The wallpaper picks its loops by it,
// and the renders are made for the sun positions a year there goes through.
//
//   node sun.js [ISO time]            the sun now, or then
//   node sun.js --year                elevation/azimuth bins a year covers

export const LONGYEARBYEN = { lat: 78.2232, lon: 15.6469 };

export function sunPosition(date, place = LONGYEARBYEN) {
    const rad = Math.PI / 180;
    const jd = date.getTime() / 86400000 + 2440587.5;
    const t = (jd - 2451545) / 36525;
    const l0 = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360;
    const m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const c = Math.sin(m * rad) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
              Math.sin(2 * m * rad) * (0.019993 - 0.000101 * t) + Math.sin(3 * m * rad) * 0.000289;
    const trueLong = l0 + c;
    const omega = 125.04 - 1934.136 * t;
    const lambda = trueLong - 0.00569 - 0.00478 * Math.sin(omega * rad);
    const eps0 = 23 + (26 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60) / 60;
    const eps = eps0 + 0.00256 * Math.cos(omega * rad);
    const decl = Math.asin(Math.sin(eps * rad) * Math.sin(lambda * rad)) / rad;
    const y = Math.tan(eps * rad / 2) ** 2;
    const eqTime = 4 / rad * (y * Math.sin(2 * l0 * rad) - 2 * e * Math.sin(m * rad) +
        4 * e * y * Math.sin(m * rad) * Math.cos(2 * l0 * rad) - 0.5 * y * y * Math.sin(4 * l0 * rad) -
        1.25 * e * e * Math.sin(2 * m * rad));
    const minutes = date.getUTCHours() * 60 + date.getUTCMinutes() + date.getUTCSeconds() / 60;
    let ha = (minutes + eqTime + 4 * place.lon) / 4 - 180;
    if (ha < -180) ha += 360;
    const lat = place.lat * rad, d = decl * rad, h = ha * rad;
    const zenith = Math.acos(Math.min(1, Math.max(-1, Math.sin(lat) * Math.sin(d) + Math.cos(lat) * Math.cos(d) * Math.cos(h))));
    let az = Math.acos(Math.min(1, Math.max(-1, (Math.sin(lat) * Math.cos(zenith) - Math.sin(d)) /
        (Math.cos(lat) * Math.sin(zenith))))) / rad;
    az = ha > 0 ? (az + 180) % 360 : (540 - az) % 360;
    return { elevation: 90 - zenith / rad, azimuth: az, declination: decl };
}

if (process.argv[1] && process.argv[1].endsWith('sun.js')) {
    if (process.argv[2] === '--year') {
        // how often each (elevation 5°, azimuth 30°) bin occurs over a year, hourly
        const bins = new Map();
        const start = Date.UTC(2026, 0, 1);
        for (let h = 0; h < 365 * 24; h++) {
            const p = sunPosition(new Date(start + h * 3600000));
            const key = `${Math.floor(p.elevation / 5) * 5}/${Math.floor(p.azimuth / 30) * 30}`;
            bins.set(key, (bins.get(key) || 0) + 1);
        }
        const rows = [...bins].sort((a, b) => b[1] - a[1]);
        console.log(`${rows.length} bins; elevation/azimuth: hours a year`);
        for (const [k, v] of rows) console.log(k.padEnd(10), v);
    } else {
        const p = sunPosition(process.argv[2] ? new Date(process.argv[2]) : new Date());
        console.log(`elevation ${p.elevation.toFixed(2)}°, azimuth ${p.azimuth.toFixed(1)}°`);
    }
}
