package dev.csgogc.mm.config;

import jakarta.servlet.FilterChain;
import jakarta.servlet.ServletException;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import java.io.IOException;
import org.springframework.web.filter.OncePerRequestFilter;

/** Turns a login attempt away before the password is even looked at while the client address is locked out. */
public class LoginThrottleFilter extends OncePerRequestFilter {

    private final LoginThrottle throttle;

    public LoginThrottleFilter(LoginThrottle throttle) {
        this.throttle = throttle;
    }

    @Override
    protected boolean shouldNotFilter(HttpServletRequest request) {
        return !"POST".equals(request.getMethod()) || !"/admin/login".equals(request.getRequestURI());
    }

    @Override
    protected void doFilterInternal(HttpServletRequest request, HttpServletResponse response, FilterChain chain)
            throws ServletException, IOException {
        if (throttle.isLocked(request.getRemoteAddr())) {
            response.sendRedirect("/admin/login?locked");
            return;
        }
        chain.doFilter(request, response);
    }
}
