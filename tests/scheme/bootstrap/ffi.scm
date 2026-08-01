;; Phase 10 MVP: dynamic library open/symbol lookup/call marshalling.

(import (scheme base))
(import (chaaya ffi))

(define libc (open-foreign-library #f))
(check-assert "foreign-library? true" (foreign-library? libc))

(define c-abs (foreign-procedure libc "abs" '(int) 'int))
(check-assert "foreign-procedure? true" (foreign-procedure? c-abs))
(check-eqv "foreign int call" 37 (c-abs -37))

(define c-atof (foreign-procedure libc "atof" '(pointer) 'double))
(check-assert "foreign double call"
              (< (abs (- (c-atof "3.25") 3.25)) 1e-9))

(define c-malloc (foreign-procedure libc "malloc" '(int) 'pointer))
(define c-free (foreign-procedure libc "free" '(pointer) 'void))
(define p (c-malloc 32))
(check-assert "foreign pointer return" (and p (integer? p)))
(c-free p)

(close-foreign-library! libc)

(check-finish)
