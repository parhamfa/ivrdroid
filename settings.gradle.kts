pluginManagement {
    repositories {
        google()
        // Availability fallback for environments where dl.google.com is blocked. Gradle's
        // committed SHA-256 verification metadata pins every artifact fetched from this mirror.
        maven("https://maven.aliyun.com/repository/google") {
            content {
                includeGroupByRegex("androidx\\..*")
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google\\.testing.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        maven("https://maven.aliyun.com/repository/google") {
            content {
                includeGroupByRegex("androidx\\..*")
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google\\.testing.*")
            }
        }
        mavenCentral()
    }
}

rootProject.name = "IVRdroid"
include(":app")
