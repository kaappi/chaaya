(import (scheme base) (srfi 64))

(test-begin "guard-unbound")
(test-assert "unbound in guard body"
  (guard (e (#t #t))
    helper
    #f))
(test-end "guard-unbound")
