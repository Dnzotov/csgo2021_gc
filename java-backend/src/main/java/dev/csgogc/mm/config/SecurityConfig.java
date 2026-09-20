package dev.csgogc.mm.config;

import jakarta.servlet.http.HttpServletResponse;
import java.util.LinkedHashMap;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.http.HttpMethod;
import org.springframework.security.config.annotation.web.builders.HttpSecurity;
import org.springframework.security.config.annotation.web.configuration.EnableWebSecurity;
import org.springframework.security.core.userdetails.User;
import org.springframework.security.core.userdetails.UserDetailsService;
import org.springframework.security.crypto.bcrypt.BCryptPasswordEncoder;
import org.springframework.security.crypto.password.PasswordEncoder;
import org.springframework.security.provisioning.InMemoryUserDetailsManager;
import org.springframework.security.web.AuthenticationEntryPoint;
import org.springframework.security.web.DefaultRedirectStrategy;
import org.springframework.security.web.SecurityFilterChain;
import org.springframework.security.web.authentication.LoginUrlAuthenticationEntryPoint;
import org.springframework.security.web.authentication.UsernamePasswordAuthenticationFilter;
import org.springframework.security.web.authentication.DelegatingAuthenticationEntryPoint;
import org.springframework.security.web.servlet.util.matcher.PathPatternRequestMatcher;
import org.springframework.security.web.util.matcher.RequestMatcher;

/**
 * Access rules:
 * <ul>
 *   <li>/admin/** and everything the panel calls: logged-in admin (session cookie, CSRF protected);</li>
 *   <li>POST /api/v1/matchmaking/search|cancel|accepted: the GC, X-Api-Key (open only if backend.api-key is empty);</li>
 *   <li>GET /api/v1/matchmaking/searches: admin session or API key;</li>
 *   <li>POST /api/v1/servers/state, GET /api/v1/servers/roster: a game server, X-Api-Key;</li>
 *   <li>GET /api/v1/health: public.</li>
 * </ul>
 */
@Configuration
@EnableWebSecurity
public class SecurityConfig {

    private static final Logger log = LoggerFactory.getLogger(SecurityConfig.class);

    @Bean
    PasswordEncoder passwordEncoder() {
        return new BCryptPasswordEncoder();
    }

    /** the StartupValidator parameter only makes sure credentials were validated before they are used here */
    @Bean
    UserDetailsService adminUser(BackendProperties props, PasswordEncoder encoder, StartupValidator validated) {
        return new InMemoryUserDetailsManager(User.withUsername(props.admin().username())
                .password(encoder.encode(props.admin().password()))
                .roles("ADMIN")
                .build());
    }

    @Bean
    SecurityFilterChain filterChain(HttpSecurity http, BackendProperties props, LoginThrottle throttle) throws Exception {
        boolean apiKeyConfigured = !props.apiKey().isBlank();

        http.authorizeHttpRequests(auth -> {
            auth.requestMatchers("/api/v1/health", "/error", "/admin/login", "/admin/assets/**", "/").permitAll();
            if (apiKeyConfigured) {
                auth.requestMatchers(HttpMethod.POST, "/api/v1/matchmaking/search", "/api/v1/matchmaking/cancel",
                                "/api/v1/matchmaking/accepted")
                        .hasAnyRole("API", "ADMIN");
            } else {
                auth.requestMatchers(HttpMethod.POST, "/api/v1/matchmaking/search", "/api/v1/matchmaking/cancel",
                                "/api/v1/matchmaking/accepted")
                        .permitAll();
            }
            auth.requestMatchers(HttpMethod.GET, "/api/v1/matchmaking/searches", "/api/v1/matchmaking/search/*").hasAnyRole("API", "ADMIN");
            // a game server (or a script) reporting its state: same key as the GC
            auth.requestMatchers(HttpMethod.POST, "/api/v1/servers/state").hasAnyRole("API", "ADMIN");
            // the srcds side asking for the roster of the match on its server (RESEARCH_FINDINGS.md #54)
            auth.requestMatchers(HttpMethod.GET, "/api/v1/servers/roster").hasAnyRole("API", "ADMIN");
            auth.requestMatchers(HttpMethod.POST, "/api/v1/servers/roster/ready").hasAnyRole("API", "ADMIN");
            auth.requestMatchers("/admin", "/admin/**").hasRole("ADMIN");
            auth.anyRequest().denyAll();
        });

        // the GC authenticates every call with the API key, no cookies, so no CSRF there; the panel is protected
        http.csrf(csrf -> csrf.ignoringRequestMatchers("/api/v1/**"));

        http.headers(headers -> headers
                .frameOptions(frame -> frame.deny())
                .contentSecurityPolicy(csp -> csp.policyDirectives(
                        "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                                + "frame-ancestors 'none'; base-uri 'none'; form-action 'self'")));

        http.formLogin(form -> form
                .loginPage("/admin/login")
                .loginProcessingUrl("/admin/login")
                .successHandler((request, response, authentication) -> {
                    throttle.success(request.getRemoteAddr());
                    new DefaultRedirectStrategy().sendRedirect(request, response, "/admin");
                })
                .failureHandler((request, response, exception) -> {
                    throttle.failure(request.getRemoteAddr());
                    log.warn("failed admin login from {}", request.getRemoteAddr());
                    new DefaultRedirectStrategy().sendRedirect(request, response,
                            throttle.isLocked(request.getRemoteAddr()) ? "/admin/login?locked" : "/admin/login?error");
                }));

        http.logout(logout -> logout
                .logoutUrl("/admin/logout")
                .logoutSuccessUrl("/admin/login?logout")
                .invalidateHttpSession(true)
                .deleteCookies("MMSESSION"));

        http.exceptionHandling(ex -> ex.authenticationEntryPoint(entryPoint()));

        http.addFilterBefore(new LoginThrottleFilter(throttle), UsernamePasswordAuthenticationFilter.class);
        http.addFilterBefore(new ApiKeyFilter(props.apiKey()), UsernamePasswordAuthenticationFilter.class);

        http.httpBasic(basic -> basic.disable());
        return http.build();
    }

    /** API calls get a JSON 401, browser pages are sent to the login form */
    private static AuthenticationEntryPoint entryPoint() {
        AuthenticationEntryPoint json = (request, response, exception) -> {
            response.setStatus(HttpServletResponse.SC_UNAUTHORIZED);
            response.setContentType("application/json");
            response.getWriter().write("{\"error\":\"unauthorized\",\"message\":\"authentication required\"}");
        };
        LinkedHashMap<RequestMatcher, AuthenticationEntryPoint> map = new LinkedHashMap<>();
        map.put(PathPatternRequestMatcher.withDefaults().matcher("/api/**"), json);
        map.put(PathPatternRequestMatcher.withDefaults().matcher("/admin/api/**"), json);
        DelegatingAuthenticationEntryPoint delegating = new DelegatingAuthenticationEntryPoint(map);
        delegating.setDefaultEntryPoint(new LoginUrlAuthenticationEntryPoint("/admin/login"));
        return delegating;
    }
}
