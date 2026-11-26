# סוכן מכירת יד שנייה

שולחים תמונות בטלגרם, הסוכן מזהה פריטים, משפר תמונות, כותב מודעה, מציע מחיר, ואחרי אישור מפרסם ומנהל מו"מ עד שהקונה מוכן. אז מתן נכנס לתיאום איסוף וכסף.

תכנון מלא: [docs/architecture.md](docs/architecture.md)

## מודל: Gemini בשכבה החינמית
כברירת מחדל הסוכן עובד עם Gemini של גוגל (`gemini-2.5-flash`, תומך בתמונות ובעברית), ובשכבה החינמית אין תשלום ואין צורך בכרטיס אשראי.
שכבת הספק מופשטת (`src/secondhand_agent/llm.py`): כדי לחזור ל-Claude משנים בהגדרות `LLM_PROVIDER=anthropic`, מוסיפים `ANTHROPIC_API_KEY`, ומתקינים `pip install -e ".[anthropic]"`. אפשר גם לבחור מודל אחר עם `LLM_MODEL`.

## מדריך מהיר: מפתח Gemini חינמי והרצת הבוט

### 1. מפתח Gemini
1. נכנסים ל-[aistudio.google.com/apikey](https://aistudio.google.com/apikey) ומתחברים עם חשבון גוגל.
2. לוחצים **Create API key** ובוחרים פרויקט (אפשר ליצור חדש).
3. מעתיקים את המפתח. אין צורך להגדיר חיוב: השכבה החינמית מוגבלת בבקשות לדקה וליום, וזה מספיק לשימוש אישי.
4. שימו לב: בשכבה החינמית גוגל עשויה להשתמש בתוכן הבקשות לשיפור המוצרים שלה, כולל התמונות. אם זה מפריע, אפשר לעבור ל-Claude.

### 2. בוט טלגרם
1. בטלגרם מדברים עם [@BotFather](https://t.me/BotFather), שולחים `/newbot` ומקבלים טוקן (`TELEGRAM_BOT_TOKEN`).
2. את המזהה שלכם (`OWNER_TELEGRAM_ID`) מקבלים מ-[@userinfobot](https://t.me/userinfobot).

### 3. הרצה
```
cd secondhand-agent
python -m venv .venv && source .venv/bin/activate
pip install -e ".[dev]"
cp .env.example .env
```
פותחים את `.env` וממלאים `GEMINI_API_KEY`, `TELEGRAM_BOT_TOKEN`, `OWNER_TELEGRAM_ID`. אחר כך:
```
python -m secondhand_agent.main
```
שולחים לבוט תמונה או כמה תמונות (אלבום) ומקבלים טיוטת מודעה עם כפתורי אישור.

בדיקות: `pytest`

## שלבים
1. MVP: תמונות ← מודעה לאישור בטלגרם (קיים)
2. פרסום ב-Facebook Marketplace וביד2 (Playwright, באישור ידני) — מתוכנן
3. ניהול פניות ומו"מ — הפרומפט כתוב, החיבור לערוצים מתוכנן
