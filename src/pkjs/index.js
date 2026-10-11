var Clay = require('./clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

// Icon ids must match the I_* enum in src/c/main.c
var I = {
  CLEAR_DAY: 0, CLEAR_NIGHT: 1, PARTLY_DAY: 2, PARTLY_NIGHT: 3, CLOUDY: 4,
  RAIN: 5, SNOW: 6, THUNDER: 7, FOG: 8, UNKNOWN: 9,
  HEAVY_RAIN: 18, HEAVY_SNOW: 19, SLEET: 20
};

// WMO weather codes -> watch icon
function iconFor(code, isDay) {
  if (code === 0) return isDay ? I.CLEAR_DAY : I.CLEAR_NIGHT;
  if (code === 1 || code === 2) return isDay ? I.PARTLY_DAY : I.PARTLY_NIGHT;
  if (code === 3) return I.CLOUDY;
  if (code === 45 || code === 48) return I.FOG;
  if (code === 56 || code === 57 || code === 66 || code === 67) return I.SLEET;
  if (code === 65 || code === 82) return I.HEAVY_RAIN;
  if ((code >= 51 && code <= 64) || (code >= 80 && code <= 81)) return I.RAIN;
  if (code === 75 || code === 86) return I.HEAVY_SNOW;
  if ((code >= 71 && code <= 77) || code === 85) return I.SNOW;
  if (code >= 95) return I.THUNDER;
  return I.UNKNOWN;
}

function get(url, done) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    try { done(JSON.parse(this.responseText)); } catch (e) { console.log('Bad response: ' + e); }
  };
  xhr.onerror = function () { console.log('Request failed: ' + url); };
  xhr.open('GET', url);
  xhr.send();
}

function locationOverride() {
  try {
    var settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
    return (settings.WeatherLocation || '').trim();
  } catch (e) {
    return '';
  }
}

var busy = false;

function fetchForecast(lat, lon) {
  // Always fetch Celsius; the watch converts so unit changes apply instantly
  var url = 'https://api.open-meteo.com/v1/forecast?latitude=' + lat + '&longitude=' + lon +
    '&current=temperature_2m,weather_code,is_day' +
    '&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto';
  get(url, function (data) {
    busy = false;
    if (!data.current) return;
    Pebble.sendAppMessage({
      WeatherTemp: Math.round(data.current.temperature_2m * 10),
      WeatherHi: Math.round(data.daily.temperature_2m_max[0] * 10),
      WeatherLo: Math.round(data.daily.temperature_2m_min[0] * 10),
      WeatherIcon: iconFor(data.current.weather_code, data.current.is_day === 1)
    }, null, function () { console.log('Weather send failed'); });
  });
}

function fetchWeather() {
  if (busy) return;
  busy = true;
  setTimeout(function () { busy = false; }, 30000);

  var place = locationOverride();
  if (place) {
    get('https://geocoding-api.open-meteo.com/v1/search?count=1&name=' + encodeURIComponent(place), function (data) {
      if (data.results && data.results.length) {
        fetchForecast(data.results[0].latitude, data.results[0].longitude);
      } else {
        busy = false;
        console.log('Location not found: ' + place);
      }
    });
    return;
  }

  navigator.geolocation.getCurrentPosition(
    function (pos) { fetchForecast(pos.coords.latitude, pos.coords.longitude); },
    function (err) { busy = false; console.log('Location error: ' + err.message); },
    { timeout: 15000, maximumAge: 600000 }
  );
}

Pebble.addEventListener('ready', fetchWeather);

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.RequestWeather) fetchWeather();
});

// Clay sends the settings itself; refresh weather in case the location changed
Pebble.addEventListener('webviewclosed', function (e) {
  if (e && e.response) setTimeout(fetchWeather, 1500);
});
