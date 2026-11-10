from .bot import build_app
from .config import load_config


def main():
    build_app(load_config()).run_polling()


if __name__ == "__main__":
    main()
