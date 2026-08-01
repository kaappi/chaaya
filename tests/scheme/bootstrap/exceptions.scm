;; Phase 7A — exceptions, guard, and parameters.

(define err (make-error "boom" 1 2))
(check-assert "error-object? true" (error-object? err))
(check-assert "error? true" (error? err))
(check-equal "error-object-message" "boom" (error-object-message err))
(check-equal "error-object-irritants" '(1 2) (error-object-irritants err))
(check-assert "file-error? false" (not (file-error? err)))
(check-assert "read-error? false" (not (read-error? err)))

(check-eqv "guard catches error object"
           'handled
           (guard (e
                    ((error-object? e) 'handled))
             (error "guard boom")))

(check-eqv "guard passthrough"
           17
           (guard (e (else 'bad))
             17))

(check-eqv "file-error? from open-input-file"
           'file
           (guard (e
                    ((file-error? e) 'file)
                    (else 'other))
             (open-input-file ".definitely-missing-phase7a.scm")
             'nope))

(check-eqv "read-error? from read"
           'read
           (guard (e
                    ((read-error? e) 'read)
                    (else 'other))
             (read (open-input-string "("))
             'nope))

(define p (make-parameter 1))
(check-eqv "make-parameter initial" 1 (p))
(p 5)
(check-eqv "make-parameter set" 5 (p))
(check-eqv "parameterize binds" 9 (parameterize ((p 9)) (p)))
(check-eqv "parameterize restores" 5 (p))

(define q (make-parameter 3 (lambda (x) (+ x 1))))
(check-eqv "converter applies at creation" 4 (q))
(q 7)
(check-eqv "converter applies on set" 8 (q))
(check-eqv "converter applies in parameterize"
           11
           (parameterize ((q 10))
             (q)))
(check-eqv "converter restore after parameterize" 8 (q))

(define out0 (current-output-port))
(check-assert "current-output-port is parameter object"
              (procedure? current-output-port))
(define out1 (open-output-string))
(parameterize ((current-output-port out1))
  (display "hello")
  (newline)
  (write 42))
(check-equal "parameterized current-output-port writes"
             "hello\n42"
             (get-output-string out1))
(check-eq "current-output-port restored" out0 (current-output-port))

(define in1 (open-input-string "41"))
(check-eqv "parameterized current-input-port read"
           41
           (parameterize ((current-input-port in1))
             (read)))

(define err0 (current-error-port))
(define err1 (open-output-string))
(check-eq "parameterized current-error-port"
          err1
          (parameterize ((current-error-port err1))
            (current-error-port)))
(check-eq "current-error-port restored" err0 (current-error-port))

(define out2 (open-output-string))
(current-output-port out2)
(display "x")
(current-output-port out0)
(check-equal "parameter call setter" "x" (get-output-string out2))

(check-finish)
