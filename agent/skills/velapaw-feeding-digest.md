# VelaPaw Feeding Digest

Report pet feeding, appetite and body condition.

## When to use
When asked about a pet, feeding, how much a pet ate,
appetite, body condition or feeder status. Also used
by the periodic heartbeat check.

## How to use
1. read_file /data/ai_agent/velapaw/status.md
Per pet it gives grams today, meal count, 7-day total,
baseline, appetite percent, body condition, daily limit.
2. read_file /data/ai_agent/velapaw/feed_log.md
Recent feed events with timestamps and match score.
3. Report per pet: grams today vs the daily limit,
appetite vs baseline, and body condition.
4. Flag a concern ONLY if any of these is true:
- appetite below 50 percent of baseline
- grams today reached the owner daily limit
- body condition is not ideal
- no feed event today in the log
5. Otherwise say the pet is on track, one sentence.
6. Use only numbers from those files. Never invent.

## Example
User: how is jay doing today
Agent: reads status.md, then replies:
Jay has had 24 g in 2 meals, 68 percent of his 35 g/day
baseline, under the 60 g limit. Body condition ideal.
