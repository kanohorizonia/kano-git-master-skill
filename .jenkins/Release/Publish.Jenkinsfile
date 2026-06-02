@Library(['kano-jenkins-common-pipeline-library@main', 'kano-jenkins-agent-skill-pipeline-library@main']) _

pipeline {
    agent none

    parameters {
        booleanParam(name: 'DRY_RUN', defaultValue: true, description: 'Render the Release/Publish plan only by default.')
        booleanParam(name: 'PUBLISH_RELEASE', defaultValue: false, description: 'Enable Release/Publish side effects such as GitHub release creation.')
        string(name: 'RELEASE_TAG', defaultValue: '', description: 'Required for non-dry-run publishing, for example v0.1.0-beta.')
    }

    stages {
        stage('Run Release/Publish') {
            steps {
                script {
                    agentSkillPipelineFromProjectConfig(
                        configPath: '.jenkins/config/agent-skill-pipeline.json',
                        bootstrapAgentLabel: 'lightweight',
                        config: agentSkillParameterProfile(
                            params: params,
                            githubRepository: 'kanohorizonia/kano-git-master-skill',
                            githubTokenCredentialId: 'github-release-token',
                            siteBuildCommand: 'pixi run docs-ci',
                            releaseApprovalMessage: 'Approve kano-git-master-skill Release/Publish? Confirm tag, artifacts, docs, and release plan before proceeding.',
                        ) + [
                            bRunBuildPhase             : false,
                            bBuildSite                 : true,
                            bPublishSite               : false,
                            bCreateGitHubRelease       : params.PUBLISH_RELEASE,
                            bRequireExplicitReleaseTag : true,
                            provisionalBuildDisplayName: "#${env.BUILD_NUMBER ?: '0'} Release/Publish queued",
                            latestQueuedBuildLockName  : 'kano-git-master-skill-release-publish',
                        ]
                    )
                }
            }
        }
    }
}
