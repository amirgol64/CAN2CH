/* DBC parser and decoder tests. Run with:  node test/dbc.test.js */
'use strict';

const assert = require('assert');
const path = require('path');
const Dbc = require(path.join(__dirname, '..', 'web', 'dbc.js'));

const SAMPLE = `VERSION ""

NS_ :

BS_:

BU_: ECU GW

BO_ 386 EngineData: 8 ECU
 SG_ EngineSpeed : 0|16@1+ (0.25,0) [0|16383.75] "rpm" GW
 SG_ CoolantTemp : 16|8@1+ (1,-40) [-40|215] "degC" GW
 SG_ Torque : 24|12@1- (0.5,0) [-1024|1023.5] "Nm" GW
 SG_ Gear : 36|4@1+ (1,0) [0|15] "" GW

BO_ 1063 MotorolaMsg: 8 ECU
 SG_ BigUnsigned : 7|16@0+ (1,0) [0|65535] "" GW
 SG_ BigSigned : 23|12@0- (1,0) [-2048|2047] "" GW
 SG_ Nibble : 27|4@0+ (1,0) [0|15] "" GW

BO_ 2566869221 ExtMsg: 8 ECU
 SG_ Wide : 0|64@1+ (1,0) [0|0] "" GW

BO_ 512 MuxMsg: 8 ECU
 SG_ Page M : 0|8@1+ (1,0) [0|255] "" GW
 SG_ ValueA m0 : 8|8@1+ (1,0) [0|255] "" GW
 SG_ ValueB m1 : 8|16@1+ (0.1,0) [0|6553.5] "V" GW

BO_ 3221225472 VECTOR__INDEPENDENT_SIG_MSG: 0 Vector__XXX
 SG_ Orphan : 0|8@1+ (1,0) [0|0] "" Vector__XXX

VAL_ 386 Gear 0 "Neutral" 1 "First" 2 "Second" ;
`;

function bytes(hex) {
  return Uint8Array.from(hex.match(/../g).map((b) => parseInt(b, 16)));
}
function byName(list) {
  const out = {};
  list.forEach((s) => { out[s.name] = s; });
  return out;
}

const db = Dbc.parse(SAMPLE);

// Messages and IDs
assert.strictEqual(db.messages.size, 4, 'four real messages (the Vector placeholder is skipped)');
assert.strictEqual(Dbc.lookup(db, 0x182, false).name, 'EngineData');
assert.strictEqual(Dbc.lookup(db, 0x182, true), null, 'standard and extended IDs are distinct');
const ext = Dbc.lookup(db, 0x18FF50E5, true);
assert.ok(ext && ext.ext, 'bit 31 of the DBC number marks an extended ID');
assert.strictEqual(Dbc.lookup(db, 0x999, false), null);
assert.strictEqual(Dbc.lookup(null, 0x182, false), null);

// Intel (little-endian) signals: scale, offset, sign, value table
let s = byName(Dbc.decode(Dbc.lookup(db, 0x182, false), bytes('401F5AFE2F000000')));
assert.strictEqual(s.EngineSpeed.value, 2000);          // 0x1F40 * 0.25
assert.strictEqual(s.EngineSpeed.unit, 'rpm');
assert.strictEqual(s.CoolantTemp.value, 50);            // 0x5A - 40
assert.strictEqual(s.Torque.value, -1);                 // 0xFFE = -2, * 0.5
assert.strictEqual(s.Gear.value, 2);
assert.strictEqual(s.Gear.text, 'Second');
assert.strictEqual(s.EngineSpeed.text, null);

// Motorola (big-endian) signals
s = byName(Dbc.decode(Dbc.lookup(db, 0x427, false), bytes('1234FFEA00000000')));
assert.strictEqual(s.BigUnsigned.value, 0x1234);
assert.strictEqual(s.BigSigned.value, -2);              // 0xFFE as 12-bit signed
assert.strictEqual(s.Nibble.value, 0xA);

// 64-bit signal keeps full precision in the raw value
const wide = ext.signals[0];
assert.strictEqual(Dbc.rawValue(wide, bytes('FFFFFFFFFFFFFFFF')), 0xFFFFFFFFFFFFFFFFn);
assert.strictEqual(Dbc.rawValue(wide, bytes('0100000000000080')), 0x8000000000000001n);

// Multiplexed signals: only the active page is shown
s = byName(Dbc.decode(Dbc.lookup(db, 0x200, false), bytes('002A000000000000')));
assert.strictEqual(s.Page.value, 0);
assert.strictEqual(s.ValueA.value, 42);
assert.ok(!('ValueB' in s));
s = byName(Dbc.decode(Dbc.lookup(db, 0x200, false), bytes('01E8030000000000')));
assert.strictEqual(s.ValueB.value, 100);                // 1000 * 0.1, no float noise
assert.ok(!('ValueA' in s));

// A frame shorter than the signal layout: decode what fits, skip the rest
s = byName(Dbc.decode(Dbc.lookup(db, 0x182, false), bytes('401F')));
assert.strictEqual(s.EngineSpeed.value, 2000);
assert.ok(!('CoolantTemp' in s) && !('Gear' in s));

// Garbage in, nothing out
assert.strictEqual(Dbc.parse('this is not a dbc file').messages.size, 0);
assert.strictEqual(Dbc.parse('').messages.size, 0);

console.log('dbc.js: all tests passed');
