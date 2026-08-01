;; Phase 6/7: R7RS libraries + import modifiers

(import (scheme base))

(check-eqv "import scheme.base +" 3 (+ 1 2))

(define-library (mylib)
  (import (scheme base))
  (export double triple (rename double twice))
  (begin
    (define (double x) (* x 2))
    (define (triple x) (* x 3))))

(import (mylib))
(check-eqv "custom lib double" 42 (double 21))
(check-eqv "custom lib triple" 30 (triple 10))
(check-eqv "export rename twice" 8 (twice 4))

(define-library (my utils)
  (import (scheme base))
  (export add5)
  (begin
    (define (add5 x) (+ x 5))))

(import (my utils))
(check-eqv "dotted name lib" 15 (add5 10))

(import (demo greet))
(check-equal "sld greet" "hello chaaya" (greet "chaaya"))
(check-eqv "sld double" 10 (double 5))

;; Import modifiers
(import (only (scheme write) display newline))
(check-assert "only display" (procedure? display))
(check-assert "only newline" (procedure? newline))

(import (except (scheme cxr) cddr))
(check-eqv "except keeps cadr" 2 (cadr '(1 2 3)))

(import (prefix (only (scheme write) write) io:))
(check-assert "prefix write" (procedure? io:write))

(import (rename (only (scheme base) +) (+ plus)))
(check-eqv "rename +" 7 (plus 3 4))

(import (scheme read))
(check-assert "scheme.read eof-object?" (procedure? eof-object?))

(import (only (scheme complex) make-rectangular real-part imag-part))
(check-assert "scheme.complex make-rectangular"
              (= (make-rectangular 1 2) 1+2i))
(check-eqv "scheme.complex real-part" 1.0 (real-part 1+2i))
(check-eqv "scheme.complex imag-part" 2.0 (imag-part 1+2i))

(check-finish)
