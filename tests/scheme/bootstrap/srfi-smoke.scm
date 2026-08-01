;; Phase 9: portable SRFI smoke (partial)

(import (scheme base))
(import (srfi 1))
(import (srfi lists-1))
(import (srfi 35))
(import (srfi 64))

(check-equal "srfi-1 iota" '(0 1 2 3 4) (iota 5))
(check-eqv "srfi-261 alias import" 10 (first '(10 20)))
(check-assert "srfi-35 condition-type?" (condition-type? &condition))
(check-assert "srfi-64 import" (procedure? test-runner-null))
(check-equal "cond-expand srfi-1 feature" 'yes
             (cond-expand (srfi-1 'yes) (else 'no)))
(check-equal "cond-expand srfi-261 feature" 'yes
             (cond-expand (srfi-261 'yes) (else 'no)))
(check-equal "cond-expand srfi-64 feature" 'yes
             (cond-expand (srfi-64 'yes) (else 'no)))

(import (srfi 13))
(import (srfi 14))
(import (srfi 69))
(check-assert "srfi-13 string-prefix?" (string-prefix? "foo" "foobar"))
(check-assert "srfi-14 char-set-contains?" (char-set-contains? char-set:full #\a))
(check-eqv "srfi-69 hash table" 42
           (let ((ht (make-hash-table eq?)))
             (hash-table-set! ht 'answer 42)
             (hash-table-ref ht 'answer)))

(check-finish)
