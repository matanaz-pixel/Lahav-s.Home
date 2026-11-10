# סוכן מכירת יד שנייה

שולחים תמונות בטלגרם, הסוכן מזהה פריטים, משפר תמונות, כותב מודעה, מציע מחיר, ואחרי אישור מפרסם ומנהל מו"מ עד שהקונה מוכן. אז מתן נכנס לתיאום איסוף וכסף.

תכנון מלא: [docs/architecture.md](docs/architecture.md)

## הרצה (שלב 1: MVP)
```
cd secondhand-agent
python -m venv .venv && source .venv/bin/activate
pip install -e ".[dev]"
cp .env.example .env   # למלא מפתחות
python -m secondhand_agent.main
```
שולחים לבוט תמונה או כמה תמונות (אלבום) ומקבלים טיוטת מודעה עם כפתורי אישור.

## שלבים
1. MVP: תמונות ← מודעה לאישור בטלגרם (קיים)
2. פרסום ב-Facebook Marketplace וביד2 (Playwright, באישור ידני) — מתוכנן
3. ניהול פניות ומו"מ — הפרומפט כתוב, החיבור לערוצים מתוכנן
