@Library(['kano-jenkins-common-pipeline-library@main', 'kano-jenkins-agent-skill-pipeline-library@main']) _

pipeline {
    agent none

    parameters {
        booleanParam(name: 'DRY_RUN', defaultValue: true, description: 'Render the QA command plan and stop before checkout/validation by default.')
        booleanParam(name: 'PUBLISH_RELEASE', defaultValue: false, description: 'Kept for standard profile compatibility; QA never publishes externally.')
        string(name: 'RELEASE_TAG', defaultValue: '', description: 'Unused by QA; present for standard profile compatibility.')
    }

    stages {
        stage('Run Agent Skill QA') {
            steps {
                script {
                    Map parameterProfile = agentSkillParameterProfile(params: params)

                    agentSkillPipeline(parameterProfile + [
                        projectName: 'kano-git-master-skill',
                        bootstrapAgentLabel: 'lightweight',
                        windowsAgentLabel: 'windows && lightweight',
                        linuxAgentLabel: 'linux && lightweight',
                        macAgentLabel: 'mac && lightweight',

                        bDryRun: params.DRY_RUN,
                        bRunBuildPhase: !params.DRY_RUN,
                        bRunReleasePhase: false,
                        bPublishSite: false,
                        bCreateGitHubRelease: false,
                        bRequireExplicitReleaseTag: false,
                        bStageSiteInputs: false,
                        bArchiveSiteArtifacts: false,

                        windowsBuildCommand: 'scripts\\kog.bat --help',
                        windowsValidateCommand: 'bash -lc "src/shell/test/pre-commit-quality-gate.sh"',
                        windowsExportCommand: 'scripts\\kog.bat --help',
                        linuxBuildCommand: './scripts/kog --help',
                        linuxValidateCommand: 'src/shell/test/pre-commit-quality-gate.sh',
                        linuxExportCommand: './scripts/kog --help',
                        macBuildCommand: './scripts/kog --help',
                        macValidateCommand: 'src/shell/test/pre-commit-quality-gate.sh',
                        macExportCommand: './scripts/kog --help',
                        bNormalizePlatformPackages: false,
                    ])
                }
            }
        }
    }
}
