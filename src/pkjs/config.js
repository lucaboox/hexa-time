// Option values must match the C_* enum in src/c/main.c
var complications = [
  { label: 'None', value: '0' },
  { label: 'Date stacked (SAT / 10 / OCT)', value: '1' },
  { label: 'Day + date (SAT / 10)', value: '2' },
  { label: 'Date (10 OCT)', value: '17' },
  { label: 'Weather: icon + temp', value: '3' },
  { label: 'Weather: icon only', value: '4' },
  { label: 'Weather: temp only', value: '5' },
  { label: 'Weather: high / low', value: '6' },
  { label: 'Battery: icon + percent', value: '7' },
  { label: 'Battery: icon only', value: '8' },
  { label: 'Battery: percent only', value: '9' },
  { label: 'Steps', value: '10' },
  { label: 'Distance', value: '11' },
  { label: 'Sleep time', value: '12' },
  { label: 'Active calories', value: '13' },
  { label: 'Heart rate', value: '14' },
  { label: 'Active time', value: '15' },
  { label: 'Bluetooth status', value: '16' }
];

function slot(key, label, def) {
  return { type: 'select', messageKey: key, label: label, defaultValue: def, options: complications };
}

module.exports = [
  { type: 'heading', defaultValue: 'Hexa Time' },
  { type: 'text', defaultValue: 'Big time in the middle, up to six complications around it.' },

  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Time' },
      {
        type: 'select', messageKey: 'TimeFont', label: 'Time font', defaultValue: '0',
        options: [
          { label: 'Bold', value: '0' },
          { label: 'Light', value: '1' },
          { label: 'Digital', value: '2' },
          { label: 'Digital bold (smaller)', value: '4' },
          { label: 'Large', value: '3' }
        ]
      },
      {
        type: 'toggle', messageKey: 'BigTime', label: 'Bigger time when a row is empty',
        description: 'If all top or all bottom complications are None, the time grows into the free space.',
        defaultValue: true
      },
      {
        type: 'toggle', messageKey: 'CenterGroup', label: 'Group time with complications',
        description: 'When only the top or only the bottom row is used, keep that row next to the time in the middle. Off: time stays centered and the row sits at the screen edge.',
        defaultValue: false
      },
      {
        type: 'toggle', messageKey: 'LeadingZero', label: 'Leading zero on hours',
        description: 'Shows 09:30 instead of 9:30.', defaultValue: false
      },
      {
        type: 'toggle', messageKey: 'ShowAmPm', label: 'Show AM/PM under the time',
        description: 'Only appears when your watch is set to 12-hour time.', defaultValue: true
      }
    ]
  },

  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Colors' },
      { type: 'color', messageKey: 'BgColor', label: 'Background', defaultValue: '000000' },
      { type: 'color', messageKey: 'TimeColor', label: 'Time text', defaultValue: 'FFFFFF' },
      { type: 'color', messageKey: 'AmPmColor', label: 'AM/PM text', defaultValue: 'AAAAAA' },
      { type: 'color', messageKey: 'CompTextColor', label: 'Complication text', defaultValue: 'FFFFFF' },
      { type: 'color', messageKey: 'CompIconColor', label: 'Complication icons', defaultValue: '00AAFF' },
      {
        type: 'select', messageKey: 'BattColors', label: 'Battery icon color', defaultValue: '2',
        description: 'Color watches only.',
        options: [
          { label: 'Same as icon color', value: '0' },
          { label: 'Icon color, red at 20% or less', value: '1' },
          { label: 'By level: green / yellow (50%) / red (20%)', value: '2' }
        ]
      },
      {
        type: 'toggle', messageKey: 'BattTextColor', label: 'Color the battery percent too',
        description: 'Battery percent text uses the same color as the battery icon.', defaultValue: false
      },
      {
        type: 'toggle', messageKey: 'WeatherColors', label: 'Colored weather icons',
        description: 'Yellow sun, grey moon and clouds, blue rain, white snow, yellow lightning. Off uses the icon color above.',
        defaultValue: true
      }
    ]
  },

  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Bars & icon backing' },
      {
        type: 'select', messageKey: 'BarStyle', label: 'Complication bars', defaultValue: '0',
        description: 'A colored area behind the top/bottom complications, a divider line next to the time, or both. With only one row in use, the bar gets taller with bigger icons and text.',
        options: [
          { label: 'None', value: '0' },
          { label: 'Background', value: '1' },
          { label: 'Line', value: '2' },
          { label: 'Background + line', value: '3' }
        ]
      },
      { type: 'color', messageKey: 'TopBarColor', label: 'Top bar background', defaultValue: '555555' },
      { type: 'color', messageKey: 'BottomBarColor', label: 'Bottom bar background', defaultValue: '555555' },
      { type: 'color', messageKey: 'LineColor', label: 'Divider line', defaultValue: 'AAAAAA' },
      {
        type: 'select', messageKey: 'IconStyle', label: 'Icon style', defaultValue: '0',
        description: 'Line art: outlined icons (from TimeStyle) that stay clean on any color. Solid: filled icons.',
        options: [
          { label: 'Line art', value: '0' },
          { label: 'Solid', value: '1' }
        ]
      },
      {
        type: 'select', messageKey: 'IconBack', label: 'Icon backing', defaultValue: '0',
        description: 'Optional extra backing behind icons. Usually not needed with line art.',
        options: [
          { label: 'None', value: '0' },
          { label: 'Circle badge', value: '1' },
          { label: 'Rounded square badge', value: '2' },
          { label: 'Outline', value: '3' }
        ]
      },
      { type: 'color', messageKey: 'IconBackColor', label: 'Badge / outline color', defaultValue: '000000' }
    ]
  },

  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Top complications' },
      { type: 'text', defaultValue: 'Slots set to None are skipped and the rest spread out to fill the row.' },
      slot('Slot0', 'Top left', '3'),
      slot('Slot1', 'Top center', '1'),
      slot('Slot2', 'Top right', '7')
    ]
  },

  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Bottom complications' },
      slot('Slot3', 'Bottom left', '10'),
      slot('Slot4', 'Bottom center', '12'),
      slot('Slot5', 'Bottom right', '14')
    ]
  },

  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Weather' },
      {
        type: 'radiogroup', messageKey: 'TempUnit', label: 'Temperature', defaultValue: '0',
        options: [{ label: 'Celsius', value: '0' }, { label: 'Fahrenheit', value: '1' }]
      },
      {
        type: 'select', messageKey: 'WeatherInterval', label: 'Update every', defaultValue: '30',
        options: [
          { label: '15 minutes', value: '15' },
          { label: '30 minutes', value: '30' },
          { label: '1 hour', value: '60' },
          { label: '2 hours', value: '120' }
        ]
      },
      {
        type: 'input', messageKey: 'WeatherLocation', label: 'Location override',
        description: 'Leave empty to use GPS. Or type a city, e.g. "Chicago".',
        defaultValue: '', attributes: { placeholder: 'Use GPS' }
      }
    ]
  },

  {
    type: 'section',
    capabilities: ['HEALTH'],
    items: [
      { type: 'heading', defaultValue: 'Health' },
      {
        type: 'radiogroup', messageKey: 'DistUnit', label: 'Distance', defaultValue: '0',
        options: [{ label: 'Kilometers', value: '0' }, { label: 'Miles', value: '1' }]
      }
    ]
  },

  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Other' },
      { type: 'toggle', messageKey: 'BtVibe', label: 'Vibrate on Bluetooth disconnect', defaultValue: true }
    ]
  },

  { type: 'submit', defaultValue: 'Save' }
];
