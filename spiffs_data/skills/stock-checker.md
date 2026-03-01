# Stock Checker                                                               
                                                                                
  Check current stock prices and market data for any company or ticker.

  ## When to use
  - User asks for a stock price or portfolio check
  - Briefings need stock data

  ## How to use
  1. If given a company name (not a ticker), first resolve the ticker:
     web_search "[company name] stock ticker symbol NASDAQ NYSE 2026"
     Extract the confirmed ticker from results before proceeding.
  2. Fetch current price:
     web_search "[TICKER] stock price today 2026"
  3. Report: ticker, price, daily % change, 52-week range, market cap
  4. For multiple stocks, list each then summarise trend

  ## Ticker gotchas
  - ARM Holdings → ARM (NASDAQ, IPO Sept 2023 — NOT the old ARMH symbol)
  - Alphabet → GOOGL, Meta → META, not FB
  - When in doubt, always resolve the ticker first (step 1) rather than guessing

  ## Example
  **User:** "What's the ARM stock price?"
  → web_search "ARM Holdings ARM stock ticker NASDAQ 2026"   ← confirm ticker =
  ARM
  → web_search "ARM stock price today 2026"

  **Response:**
  - **ARM** (ARM Holdings): $142.50 (+1.8% today)
  - Market Cap: ~$1.5T
  - 52-week range: $95 – $175