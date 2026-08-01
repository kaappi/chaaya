;; Phase 9: portable SRFI smoke (partial)

(import (scheme base))
(import (srfi 1))
(import (srfi lists-1))

(check-equal "srfi-1 iota" '(0 1 2 3 4) (iota 5))
(check-eqv "srfi-261 alias import" 10 (first '(10 20)))
(check-equal "cond-expand srfi-1 feature" 'yes
             (cond-expand (srfi-1 'yes) (else 'no)))
(check-equal "cond-expand srfi-261 feature" 'yes
             (cond-expand (srfi-261 'yes) (else 'no)))

(check-finish)
