/*
 * Minimal DBC parser and signal decoder.
 * Works in the browser (window.Dbc) and in Node (module.exports) for tests.
 */
(function (root) {
  'use strict';

  var SG_RE = /^SG_\s+(\w+)\s*(M|m\d+M?)?\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(\s*([^,]+),\s*([^)]+)\)\s*\[\s*([^|]*)\|\s*([^\]]*)\]\s*"([^"]*)"/;
  var BO_RE = /^BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)/;
  var VAL_RE = /^VAL_\s+(\d+)\s+(\w+)\s+(.*);/;

  function key(id, ext) {
    return (ext ? 'x' : 's') + id;
  }

  /* DBC message numbers carry the extended-frame flag in bit 31. */
  function splitId(raw) {
    var ext = raw >= 0x80000000;
    var id = ext ? raw - 0x80000000 : raw;
    if (id > 0x1FFFFFFF || (!ext && id > 0x7FF)) return null;
    return { id: id, ext: ext };
  }

  function parse(text) {
    var messages = new Map();
    var cur = null;
    var lines = text.split(/\r?\n/);
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i].trim();
      var m;
      if (line === '') {
        cur = null;
      } else if ((m = BO_RE.exec(line))) {
        var parts = splitId(Number(m[1]));
        cur = null;
        if (parts) {
          cur = { id: parts.id, ext: parts.ext, name: m[2], dlc: Number(m[3]), signals: [] };
          messages.set(key(parts.id, parts.ext), cur);
        }
      } else if (cur && (m = SG_RE.exec(line))) {
        var mux = m[2] || '';
        cur.signals.push({
          name: m[1],
          isMux: mux.indexOf('M') >= 0,
          muxVal: mux[0] === 'm' ? parseInt(mux.slice(1), 10) : null,
          start: Number(m[3]),
          len: Number(m[4]),
          little: m[5] === '1',
          signed: m[6] === '-',
          factor: Number(m[7]),
          offset: Number(m[8]),
          unit: m[11],
          values: null
        });
      } else if ((m = VAL_RE.exec(line))) {
        var target = splitId(Number(m[1]));
        var msg = target && messages.get(key(target.id, target.ext));
        if (!msg) continue;
        for (var s = 0; s < msg.signals.length; s++) {
          if (msg.signals[s].name !== m[2]) continue;
          var table = {};
          var pair, re = /(-?\d+)\s+"([^"]*)"/g;
          while ((pair = re.exec(m[3]))) table[pair[1]] = pair[2];
          msg.signals[s].values = table;
        }
      }
    }
    return { messages: messages };
  }

  function lookup(db, id, ext) {
    return db ? db.messages.get(key(id, !!ext)) || null : null;
  }

  /* Raw integer value of one signal, or null if the frame is too short. */
  function rawValue(sig, data) {
    var raw = 0n;
    var i, bit, byte;
    if (sig.len < 1 || sig.len > 64) return null;
    if (sig.little) {
      for (i = 0; i < sig.len; i++) {
        bit = sig.start + i;
        byte = bit >> 3;
        if (byte >= data.length) return null;
        if ((data[byte] >> (bit & 7)) & 1) raw |= 1n << BigInt(i);
      }
    } else {
      /* Motorola: start is the most significant bit; walk towards the LSB. */
      bit = sig.start;
      for (i = 0; i < sig.len; i++) {
        byte = bit >> 3;
        if (byte >= data.length) return null;
        raw = (raw << 1n) | BigInt((data[byte] >> (bit & 7)) & 1);
        bit = (bit & 7) === 0 ? bit + 15 : bit - 1;
      }
    }
    if (sig.signed && ((raw >> BigInt(sig.len - 1)) & 1n)) raw -= 1n << BigInt(sig.len);
    return raw;
  }

  /* Returns [{name, value, unit, text}] for the signals present in the frame. */
  function decode(msg, data) {
    var out = [];
    var muxValue = null;
    var i, sig, raw;
    for (i = 0; i < msg.signals.length; i++) {
      sig = msg.signals[i];
      if (sig.isMux && sig.muxVal === null) {
        raw = rawValue(sig, data);
        if (raw !== null) muxValue = Number(raw);
      }
    }
    for (i = 0; i < msg.signals.length; i++) {
      sig = msg.signals[i];
      if (sig.muxVal !== null && sig.muxVal !== muxValue) continue;
      raw = rawValue(sig, data);
      if (raw === null) continue;
      var value = Number((Number(raw) * sig.factor + sig.offset).toPrecision(12));
      var text = sig.values ? sig.values[raw.toString()] : undefined;
      out.push({ name: sig.name, value: value, unit: sig.unit, text: text === undefined ? null : text });
    }
    return out;
  }

  var api = { parse: parse, lookup: lookup, decode: decode, rawValue: rawValue };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Dbc = api;
})(typeof self !== 'undefined' ? self : this);
