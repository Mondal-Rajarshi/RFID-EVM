// ═══════════════════════════════════════════════════════════════
//  VoteTrack — Google Apps Script Backend  (v2 — ESP8266 ready)
//  Changes from v1:
//  + Added action: reset_log  → logs a RESET event row
//  + handleReset() now adds an audit row instead of deleting
//  + handleVote() is unchanged — ESP8266 calls it the same way
// ═══════════════════════════════════════════════════════════════

// ───────────────────────────────────────────────────────────────
//  ★ EDIT ZONE 1 — Google Sheet ID
// ───────────────────────────────────────────────────────────────
const SHEET_ID = 'YOUR_SHEET_ID_HERE';   // ← change

// ───────────────────────────────────────────────────────────────
//  ★ EDIT ZONE 2 — Sheet Tab Names
// ───────────────────────────────────────────────────────────────
const SHEET_VOTES      = 'Votes';
const SHEET_CANDIDATES = 'Candidates';

// ───────────────────────────────────────────────────────────────
//  ★ EDIT ZONE 3 — Candidate Names (must match .ino file)
// ───────────────────────────────────────────────────────────────
const CANDIDATES = [
  'Araf',    // ← change
  'Alif',    // ← change
  'Raj'      // ← change
];

// ───────────────────────────────────────────────────────────────
//  ★ EDIT ZONE 4 — Timezone
// ───────────────────────────────────────────────────────────────
const TIMEZONE = 'Asia/Dhaka';   // ← change

// ═══════════════════════════════════════════════════════════════
//  COLUMN INDICES — Votes sheet (zero-based, A=0)
//  A = Candidate   (or "— RESET —" for reset events)
//  B = Voted At    (human-readable)
//  C = ISO         (full ISO timestamp)
//  D = Type        ("vote" | "reset")  ← NEW column
// ═══════════════════════════════════════════════════════════════
const COL = {
  CANDIDATE:  0,   // A
  TIMESTAMP:  1,   // B
  ISO:        2,   // C
  TYPE:       3    // D  ← NEW: "vote" or "reset"
};

// ═══════════════════════════════════════════════════════════════
//  SHEET SETUP — Run ONCE manually in Apps Script editor
// ═══════════════════════════════════════════════════════════════
function setupSheets() {
  const ss = SpreadsheetApp.openById(SHEET_ID);

  // ── Votes sheet ─────────────────────────────────────────────
  let votes = ss.getSheetByName(SHEET_VOTES);
  if (!votes) votes = ss.insertSheet(SHEET_VOTES);
  votes.clearContents();

  // 4 columns now (added "Type")
  votes.getRange(1, 1, 1, 4).setValues([[
    'Candidate', 'Voted At', 'ISO Timestamp', 'Type'
  ]]);
  votes.getRange(1, 1, 1, 4)
    .setFontWeight('bold')
    .setBackground('#0f1b2d')
    .setFontColor('#f0c040');
  votes.setFrozenRows(1);
  votes.setColumnWidth(1, 160);
  votes.setColumnWidth(2, 180);
  votes.setColumnWidth(3, 220);
  votes.setColumnWidth(4, 80);

  // ── Candidates sheet ─────────────────────────────────────────
  let cands = ss.getSheetByName(SHEET_CANDIDATES);
  if (!cands) cands = ss.insertSheet(SHEET_CANDIDATES);
  cands.clearContents();
  cands.getRange(1, 1, 1, 1).setValues([['Name']]);
  cands.getRange(1, 1, 1, 1)
    .setFontWeight('bold')
    .setBackground('#0f1b2d')
    .setFontColor('#f0c040');
  cands.setFrozenRows(1);

  CANDIDATES.forEach((name, i) => {
    cands.getRange(i + 2, 1).setValue(name);
  });

  SpreadsheetApp.flush();
  Logger.log('✅ Sheets created and candidates seeded.');
}

// ═══════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════
function getSheet(name) {
  return SpreadsheetApp.openById(SHEET_ID).getSheetByName(name);
}

function jsonOk(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

function jsonErr(msg) {
  return ContentService
    .createTextOutput(JSON.stringify({ status: 'error', message: msg }))
    .setMimeType(ContentService.MimeType.JSON);
}

function nowStr() {
  return Utilities.formatDate(new Date(), TIMEZONE, 'yyyy-MM-dd HH:mm:ss');
}

// ═══════════════════════════════════════════════════════════════
//  doGet — ROUTER
// ═══════════════════════════════════════════════════════════════
function doGet(e) {
  const params = e && e.parameter ? e.parameter : {};
  const action = (params.action || '').toLowerCase().trim();

  try {
    switch (action) {
      case 'vote':       return handleVote(params);
      case 'results':    return handleResults();
      case 'candidates': return handleCandidates();
      case 'reset_log':  return handleResetLog();   // ← called by ESP8266
      case 'reset':      return handleReset(params);// ← manual browser reset
      default:           return jsonErr('Unknown action: ' + action);
    }
  } catch (err) {
    Logger.log('doGet error: ' + err.message);
    return jsonErr('Internal error: ' + err.message);
  }
}

// ═══════════════════════════════════════════════════════════════
//  ACTION: vote
//  Called by ESP8266: SCRIPT_URL?action=vote&candidate=NAME
// ═══════════════════════════════════════════════════════════════
function handleVote(params) {
  const candidate = (params.candidate || '').trim();
  if (!candidate) return jsonErr('Missing candidate param');

  // Validate against Candidates sheet
  const sheet = getSheet(SHEET_CANDIDATES);
  if (!sheet) return jsonErr('Candidates sheet not found. Run setupSheets().');

  const names = sheet.getDataRange().getValues()
    .slice(1)
    .map(r => r[0].toString().trim());

  if (!names.includes(candidate)) {
    return jsonErr('Invalid candidate: ' + candidate +
      '. Known: ' + names.join(', '));
  }

  // Append vote row (4 columns now)
  const votes = getSheet(SHEET_VOTES);
  if (!votes) return jsonErr('Votes sheet not found. Run setupSheets().');

  votes.appendRow([
    candidate,
    nowStr(),
    new Date().toISOString(),
    'vote'          // ← Type column
  ]);
  SpreadsheetApp.flush();
  Logger.log('Vote cast for: ' + candidate);

  return jsonOk({
    status:    'ok',
    action:    'voted',
    candidate: candidate,
    voted_at:  nowStr()
  });
}

// ═══════════════════════════════════════════════════════════════
//  ACTION: reset_log
//  Called by ESP8266 after admin 5s hold reset.
//  Appends a special RESET audit row — does NOT delete votes.
//  The dashboard's handleResults() skips rows with type='reset'.
// ═══════════════════════════════════════════════════════════════
function handleResetLog() {
  const votes = getSheet(SHEET_VOTES);
  if (!votes) return jsonErr('Votes sheet not found. Run setupSheets().');

  votes.appendRow([
    '— RESET —',
    nowStr(),
    new Date().toISOString(),
    'reset'         // ← Type column marks this as a reset event
  ]);
  SpreadsheetApp.flush();

  // Highlight the reset row in red so it stands out visually
  const lastRow = votes.getLastRow();
  votes.getRange(lastRow, 1, 1, 4)
    .setBackground('#3d0000')
    .setFontColor('#ff6b6b');

  Logger.log('Reset event logged at ' + nowStr());

  return jsonOk({
    status:     'ok',
    action:     'reset_logged',
    logged_at:  nowStr()
  });
}

// ═══════════════════════════════════════════════════════════════
//  ACTION: results
//  Returns vote counts per candidate.
//  Skips rows where Type = 'reset' so reset rows don't corrupt
//  the count, and only counts votes AFTER the last reset event.
// ═══════════════════════════════════════════════════════════════
function handleResults() {
  const candSheet  = getSheet(SHEET_CANDIDATES);
  const votesSheet = getSheet(SHEET_VOTES);

  if (!candSheet)  return jsonErr('Candidates sheet not found.');
  if (!votesSheet) return jsonErr('Votes sheet not found.');

  const names = candSheet.getDataRange().getValues()
    .slice(1)
    .map(r => r[0].toString().trim())
    .filter(Boolean);

  const allRows = votesSheet.getDataRange().getValues().slice(1);

  // ── Find the index of the last RESET row ────────────────────
  // Votes before the last reset are no longer counted.
  let lastResetIdx = -1;
  allRows.forEach((r, i) => {
    if ((r[COL.TYPE] || '').toString().toLowerCase().trim() === 'reset') {
      lastResetIdx = i;
    }
  });

  // Only count rows AFTER the last reset row
  const activeRows = allRows.slice(lastResetIdx + 1);

  // ── Count votes ──────────────────────────────────────────────
  const counts = {};
  names.forEach(n => counts[n] = 0);

  let lastUpdated = null;

  activeRows.forEach(r => {
    const type = (r[COL.TYPE] || '').toString().toLowerCase().trim();
    if (type !== 'vote') return;   // skip non-vote rows

    const cand = r[COL.CANDIDATE].toString().trim();
    if (counts[cand] !== undefined) counts[cand]++;

    const ts = r[COL.TIMESTAMP].toString();
    if (!lastUpdated || ts > lastUpdated) lastUpdated = ts;
  });

  const total = activeRows.filter(r =>
    (r[COL.TYPE] || '').toString().toLowerCase().trim() === 'vote'
  ).length;

  const results = names.map(name => ({
    candidate: name,
    votes:     counts[name],
    percent:   total > 0 ? Math.round((counts[name] / total) * 100) : 0
  }));

  results.sort((a, b) => b.votes - a.votes);

  return jsonOk({
    status:       'ok',
    total:        total,
    last_updated: lastUpdated || '—',
    results:      results
  });
}

// ═══════════════════════════════════════════════════════════════
//  ACTION: candidates
// ═══════════════════════════════════════════════════════════════
function handleCandidates() {
  const sheet = getSheet(SHEET_CANDIDATES);
  if (!sheet) return jsonErr('Candidates sheet not found. Run setupSheets().');

  const names = sheet.getDataRange().getValues()
    .slice(1)
    .map(r => r[0].toString().trim())
    .filter(Boolean);

  return jsonOk({ status: 'ok', candidates: names });
}

// ═══════════════════════════════════════════════════════════════
//  ACTION: reset  (⚠️ MANUAL BROWSER TOOL — dev use only)
//  Deletes ALL rows including votes and reset events.
//  Call: SCRIPT_URL?action=reset&confirm=yes
// ═══════════════════════════════════════════════════════════════
function handleReset(params) {
  if (!params || params.confirm !== 'yes') {
    return jsonErr('Add &confirm=yes to reset. This deletes ALL rows.');
  }

  const sheet = getSheet(SHEET_VOTES);
  if (!sheet) return jsonErr('Votes sheet not found.');

  const lastRow = sheet.getLastRow();
  if (lastRow > 1) sheet.deleteRows(2, lastRow - 1);
  SpreadsheetApp.flush();

  Logger.log('⚠️ All rows cleared via browser reset.');
  return jsonOk({ status: 'ok', action: 'reset', message: 'All rows cleared.' });
}

// ═══════════════════════════════════════════════════════════════
//  doPost — accepts JSON body { action, candidate }
// ═══════════════════════════════════════════════════════════════
function doPost(e) {
  try {
    let params = {};
    if (e.postData && e.postData.contents) {
      params = JSON.parse(e.postData.contents);
    } else if (e.parameter) {
      params = e.parameter;
    }
    const action = (params.action || 'vote').toLowerCase().trim();
    if (action === 'vote')      return handleVote(params);
    if (action === 'reset_log') return handleResetLog();
    return jsonErr('POST only supports action=vote or reset_log');
  } catch (err) {
    Logger.log('doPost error: ' + err.message);
    return jsonErr('POST parse error: ' + err.message);
  }
}
