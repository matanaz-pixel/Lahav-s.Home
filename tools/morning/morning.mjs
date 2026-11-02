#!/usr/bin/env node
// לקוח CLI ל-Morning (חשבונית ירוקה) — ללא תלויות, Node 18+.
// מפתחות נקראים רק ממשתני סביבה: MORNING_API_ID, MORNING_API_SECRET
// MORNING_ENV=sandbox לסביבת בדיקות (ברירת מחדל: production).
// פעולות כתיבה רצות כ-dry-run (מדפיסות מה ישלחו) אלא אם נוסף --confirm.

const BASE = process.env.MORNING_ENV === 'sandbox'
  ? 'https://sandbox.d.greeninvoice.co.il/api/v1'
  : 'https://api.greeninvoice.co.il/api/v1';

const DOC_TYPES = {
  quote: 10, order: 100, delivery: 200, return: 210, deal: 300,
  invoice: 305, 'invoice-receipt': 320, credit: 330, receipt: 400,
};

let token;
async function api(method, path, body) {
  if (!token && path !== '/account/token') {
    const { MORNING_API_ID: id, MORNING_API_SECRET: secret } = process.env;
    if (!id || !secret) throw new Error('חסרים MORNING_API_ID / MORNING_API_SECRET');
    token = (await api('POST', '/account/token', { id, secret })).token;
  }
  const res = await fetch(BASE + path, {
    method,
    headers: { 'Content-Type': 'application/json', ...(token && { Authorization: `Bearer ${token}` }) },
    body: body && JSON.stringify(body),
  });
  const text = await res.text();
  const data = text ? JSON.parse(text) : {};
  if (!res.ok) throw new Error(`${res.status} ${method} ${path}: ${text}`);
  return data;
}

const args = process.argv.slice(2);
const flag = (n) => args.includes(`--${n}`);
const opt = (n) => { const i = args.indexOf(`--${n}`); return i > -1 ? args[i + 1] : undefined; };
const out = (x) => console.log(JSON.stringify(x, null, 2));

async function write(method, path, body) {
  if (!flag('confirm')) {
    console.log(`[dry-run] ${method} ${BASE}${path}`); out(body);
    console.log('להרצה אמיתית הוסיפו --confirm');
    return;
  }
  out(await api(method, path, body));
}

const [cmd, sub] = args;
const usage = `שימוש:
  me                                    בדיקת חיבור (פרטי העסק)
  clients search [--name X]             חיפוש לקוחות
  suppliers search [--name X]           חיפוש ספקים
  docs search [--type invoice] [--from YYYY-MM-DD] [--to YYYY-MM-DD]
  docs create --file doc.json [--confirm]    הפקת מסמך (דורש --confirm)
  client create --file client.json [--confirm]
  supplier create --file supplier.json [--confirm]
סוגי מסמך: ${Object.keys(DOC_TYPES).join(', ')}`;

const readJson = async (f) => JSON.parse(await (await import('node:fs/promises')).readFile(f, 'utf8'));

try {
  if (cmd === 'me') out(await api('GET', '/businesses/me'));
  else if (cmd === 'clients' && sub === 'search')
    out(await api('POST', '/clients/search', { name: opt('name'), page: 0, pageSize: 50 }));
  else if (cmd === 'suppliers' && sub === 'search')
    out(await api('POST', '/suppliers/search', { name: opt('name'), page: 0, pageSize: 50 }));
  else if (cmd === 'docs' && sub === 'search') {
    const t = opt('type');
    out(await api('POST', '/documents/search', {
      page: 1, pageSize: 50,
      ...(t && { type: [DOC_TYPES[t] ?? Number(t)] }),
      ...(opt('from') && { fromDate: opt('from') }),
      ...(opt('to') && { toDate: opt('to') }),
    }));
  } else if (cmd === 'docs' && sub === 'create') await write('POST', '/documents', await readJson(opt('file')));
  else if (cmd === 'client' && sub === 'create') await write('POST', '/clients', await readJson(opt('file')));
  else if (cmd === 'supplier' && sub === 'create') await write('POST', '/suppliers', await readJson(opt('file')));
  else console.log(usage);
} catch (e) { console.error(e.message); process.exit(1); }
