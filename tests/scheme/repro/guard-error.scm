(import (scheme base))
(display (guard (e ((error-object? e) 'handled))
  (error "boom")))
(newline)
