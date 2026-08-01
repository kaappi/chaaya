;; Phase 7: include, cond-expand, features, more scheme libs

(import (scheme base))

(check-assert "features list" (list? (features)))
(check-assert "r7rs feature" (and (memq 'r7rs (features)) #t))
(check-assert "chaaya feature" (and (memq 'chaaya (features)) #t))

(check-eqv "expr cond-expand r7rs" 'yes
  (cond-expand (r7rs 'yes) (else 'no)))
(check-eqv "expr cond-expand and/not" 'ok
  (cond-expand ((and r7rs (not no-such-feature)) 'ok) (else 'no)))
(check-eqv "expr cond-expand library" 'has-base
  (cond-expand ((library (scheme base)) 'has-base) (else 'no)))
(check-eqv "expr cond-expand else" 'fallback
  (cond-expand (no-such-feature 'no) (else 'fallback)))

(define-library (inc demo)
  (import (scheme base))
  (export inc-helper)
  (include "fixtures/inc-body.scm"))

(import (inc demo))
(check-eqv "library include" 6 (inc-helper 5))

(define-library (ild demo)
  (import (scheme base))
  (include-library-declarations "fixtures/ild-decls.scm"))

(import (ild demo))
(check-eqv "include-library-declarations" 'from-ild (greeter))

(define-library (ci demo)
  (import (scheme base))
  (export folded-add)
  (include-ci "fixtures/inc-ci-upper.scm"))

(import (ci demo))
(check-eqv "include-ci folds identifiers" 7 (folded-add 3 4))

(define-library (ce demo)
  (import (scheme base))
  (export which)
  (cond-expand
    (chaaya (begin (define which 'chaaya-branch)))
    (else (begin (define which 'else-branch)))))

(import (ce demo))
(check-eqv "library cond-expand" 'chaaya-branch which)

(import (scheme process-context))
(check-assert "command-line" (pair? (command-line)))
(check-assert "process features" (list? (features)))

(import (scheme char))
(check-assert "scheme.char" (char? #\a))

(check-finish)
