import asyncio
import tempfile
from pathlib import Path

from telegram import InlineKeyboardButton, InlineKeyboardMarkup, Update
from telegram.ext import Application, CallbackQueryHandler, CommandHandler, ContextTypes, MessageHandler, filters

from .config import Config
from .db import DB
from .llm import LLM
from .pipeline import process_photos

ALBUM_WAIT_SECONDS = 4  # המתנה לאיסוף כל תמונות האלבום


def build_app(cfg: Config) -> Application:
    llm, db = LLM(cfg.anthropic_key, cfg.model), DB(cfg.data_dir / "agent.db")
    app = Application.builder().token(cfg.telegram_token).build()
    owner = filters.User(user_id=cfg.owner_id)
    pending: dict[int, dict] = {}  # chat_id -> {photos, notes, task}

    async def start(update: Update, _):
        await update.message.reply_text("שלח לי תמונה או כמה תמונות של פריטים למכירה (אפשר להוסיף כיתוב עם הערות).")

    async def flush(chat_id: int, ctx: ContextTypes.DEFAULT_TYPE):
        await asyncio.sleep(ALBUM_WAIT_SECONDS)
        batch = pending.pop(chat_id, None)
        if not batch:
            return
        await ctx.bot.send_message(chat_id, f"קיבלתי {len(batch['photos'])} תמונות, מנתח...")
        out_dir = cfg.data_dir / "images"
        try:
            ids = await asyncio.to_thread(process_photos, llm, db, batch["photos"], out_dir, batch["notes"])
        except Exception as e:  # noqa: BLE001
            await ctx.bot.send_message(chat_id, f"נכשלתי בעיבוד: {e}")
            return
        for item_id in ids:
            it = db.get_item(item_id)
            ad, pr = it["data"]["listing"], it["data"]["pricing"]
            text = (
                f"*{ad['title']}*\nמחיר מוצע: {it['price']} ₪ (רצפה פנימית: {it['floor_price']} ₪)\n"
                f"ביטחון בתמחור: {pr.get('confidence')} - {pr.get('reasoning', '')}\n\n{ad['description']}"
            )
            kb = InlineKeyboardMarkup([[
                InlineKeyboardButton("✅ אשר", callback_data=f"ok:{item_id}"),
                InlineKeyboardButton("🗑 דחה", callback_data=f"no:{item_id}"),
            ]])
            first = it["data"]["images"][:1]
            if first:
                await ctx.bot.send_photo(chat_id, Path(first[0]).read_bytes())
            await ctx.bot.send_message(chat_id, text, reply_markup=kb)

    async def on_photo(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
        chat_id = update.effective_chat.id
        tmp = Path(tempfile.mkdtemp(prefix="sh_"))
        tg_file = await update.message.photo[-1].get_file()
        path = tmp / f"{update.message.message_id}.jpg"
        await tg_file.download_to_drive(path)
        batch = pending.setdefault(chat_id, {"photos": [], "notes": "", "task": None})
        batch["photos"].append(path)
        if update.message.caption:
            batch["notes"] += update.message.caption + "\n"
        if batch["task"]:
            batch["task"].cancel()
        batch["task"] = asyncio.create_task(flush(chat_id, ctx))

    async def on_button(update: Update, _):
        q = update.callback_query
        await q.answer()
        action, item_id = q.data.split(":")
        # שלב 1: רק מסמן סטטוס. שלב 2 יפעיל את הפרסום (Playwright).
        db.set_status(int(item_id), "draft" if action == "ok" else "archived")
        await q.edit_message_reply_markup(None)
        await q.message.reply_text("אושר, נשמר כטיוטה מוכנה לפרסום." if action == "ok" else "נדחה.")

    app.add_handler(CommandHandler("start", start, filters=owner))
    app.add_handler(MessageHandler(owner & filters.PHOTO, on_photo))
    app.add_handler(CallbackQueryHandler(on_button))
    return app
