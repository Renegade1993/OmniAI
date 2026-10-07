// portals.js MAP [x,y,l ...]: every monolith, subterranean gate and whirlpool of a map with its channel key, and for each
// start (or each x,y,l given) the portals within 15 tiles. October 7th (OmniAI Dev: which seat starts beside a monolith,
// and where each one leads). Uses mapsight's loader. Read-only.
const sight = require('C:/AI Projects/Heroes 3/.tmp/opus-wt/tools/mapsight/lib/sight');
const file = process.argv[2];
const M = sight.load(file);
const objs = M.objects || M.objs || [];
const key = o => o.type === 'subterraneanGate' ? 'gate' : o.type.replace(/(Entrance|Exit)$/, '') + ':' + (o.subtype || '');
const ports = objs.filter(o => /^monolith|^subterraneanGate|^whirlpool/.test(o.type));
const byKey = {};
for (const o of ports) (byKey[key(o)] = byKey[key(o)] || []).push(o);
const pos = o => '(' + (o.at || o.pos || [o.x, o.y]).slice(0, 2).join(',') + ',' + (o.l !== undefined ? o.l : o.z) + ')';
console.log('portals by channel:');
for (const k of Object.keys(byKey).sort()) console.log('  ' + k + ': ' + byKey[k].map(o => o.type.replace('monolith', 'm') + pos(o)).join(' '));
if (!objs.length) console.log('no objects found; keys of M: ' + Object.keys(M).join(','));
