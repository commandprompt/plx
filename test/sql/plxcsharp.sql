-- plxcsharp regression tests
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

SELECT lanname, lanpltrusted FROM pg_language WHERE lanname = 'plxcsharp';
