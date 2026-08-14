package auth

import (
	"context"
	"net/http"
	"strings"
)

// ctxKey is a private type for context keys owned by this package.
//
// Why not a plain string? If two packages both stored values under the string
// key "claims", they would silently collide in the same context. A private
// unexported type guarantees uniqueness — only this package can create or read
// the key, which is the Go idiom for request-scoped values (context.Context).
type ctxKey int

const claimsKey ctxKey = 0

// roleLevel maps roles to a numeric rank for "is this role sufficient?" checks.
// viewer=0, operator=1, admin=2. A role is sufficient if its level >= minimum.
func roleLevel(r Role) int {
	switch r {
	case RoleViewer:
		return 0
	case RoleOperator:
		return 1
	case RoleAdmin:
		return 2
	default:
		return -1
	}
}

// RequireRole returns an http.Handler middleware that enforces JWT authentication
// and role-based authorization.
//
// Flow:
//  1. Reads "Authorization: Bearer <token>" header.
//  2. Verifies the JWT signature and expiry.
//  3. Rejects with 401 if the token is missing or invalid.
//  4. Rejects with 403 if the token's role is below minimum.
//  5. Stores Claims in the request context for downstream handlers.
func RequireRole(secret string, minimum Role, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		header := r.Header.Get("Authorization")
		if !strings.HasPrefix(header, "Bearer ") {
			http.Error(w, "missing or malformed Authorization header", http.StatusUnauthorized)
			return
		}
		tokenStr := strings.TrimPrefix(header, "Bearer ")

		claims, err := Verify(secret, tokenStr)
		if err != nil {
			http.Error(w, "invalid or expired token", http.StatusUnauthorized)
			return
		}

		if roleLevel(claims.Role) < roleLevel(minimum) {
			http.Error(w, "insufficient role", http.StatusForbidden)
			return
		}

		// Store claims in context so downstream handlers can read username/role
		// without re-parsing the token. This is the standard Go pattern for
		// passing request-scoped data through the call chain.
		ctx := context.WithValue(r.Context(), claimsKey, claims)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

// ClaimsFromContext retrieves the Claims stored by RequireRole middleware.
// Returns (nil, false) if the context carries no claims (e.g., unauthenticated
// routes or tests that bypass middleware).
func ClaimsFromContext(ctx context.Context) (*Claims, bool) {
	c, ok := ctx.Value(claimsKey).(*Claims)
	return c, ok
}
